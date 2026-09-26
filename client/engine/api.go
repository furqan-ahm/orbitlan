package main

import (
	"crypto/subtle"
	"encoding/json"
	"net/http"
	"sync"
)

// controlAPI is the tiny local surface the UI polls. Loopback-only; not exposed off-box.
type controlAPI struct {
	mesh         *Mesh
	code         string
	myID         string
	myName       string
	myIP         string
	token        string
	shutdown     chan struct{}
	shutdownOnce sync.Once
}

func (a *controlAPI) serve(addr string) {
	mux := http.NewServeMux()
	mux.HandleFunc("/status", a.status)
	mux.HandleFunc("/health", a.health)
	mux.HandleFunc("/shutdown", a.stop)
	mux.HandleFunc("/kick", a.kick)
	http.ListenAndServe(addr, mux)
}

func (a *controlAPI) authorized(r *http.Request) bool {
	provided := r.Header.Get("X-OrbitLan-Token")
	return a.token != "" && len(provided) == len(a.token) &&
		subtle.ConstantTimeCompare([]byte(provided), []byte(a.token)) == 1
}

func (a *controlAPI) requireAuth(w http.ResponseWriter, r *http.Request) bool {
	if a.authorized(r) {
		return true
	}
	http.Error(w, "unauthorized", http.StatusUnauthorized)
	return false
}

func (a *controlAPI) health(w http.ResponseWriter, r *http.Request) {
	if !a.requireAuth(w, r) {
		return
	}
	w.Write([]byte("ok"))
}

func (a *controlAPI) stop(w http.ResponseWriter, r *http.Request) {
	if !a.requireAuth(w, r) {
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	a.shutdownOnce.Do(func() { close(a.shutdown) })
	w.Write([]byte("stopping"))
}

type statusResp struct {
	MyID       string       `json:"myID"`
	MyName     string       `json:"myName"`
	MyIP       string       `json:"myIP"`
	Code       string       `json:"code"`
	Peers      []PeerStatus `json:"peers"`
	Summary    string       `json:"summary"`
	IsHost     bool         `json:"isHost"`
	HostPeerID string       `json:"hostPeerID"`
}

func (a *controlAPI) status(w http.ResponseWriter, r *http.Request) {
	if !a.requireAuth(w, r) {
		return
	}
	peers := a.mesh.snapshotPeers()
	direct, relay, connecting := 0, 0, 0
	for _, p := range peers {
		switch p.State {
		case string(StDirect):
			direct++
		case string(StRelay):
			relay++
		default:
			connecting++
		}
	}
	isHost, hostPeerID := a.mesh.hostStatus()
	resp := statusResp{
		MyID: a.myID, MyName: a.myName, MyIP: a.myIP, Code: a.code, Peers: peers,
		Summary: summarize(direct, relay, connecting), IsHost: isHost, HostPeerID: hostPeerID,
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func validPeerID(value string) bool {
	if len(value) < 1 || len(value) > 128 {
		return false
	}
	for _, ch := range value {
		if (ch < 'a' || ch > 'z') && (ch < 'A' || ch > 'Z') &&
			(ch < '0' || ch > '9') && ch != '-' && ch != '_' {
			return false
		}
	}
	return true
}

func (a *controlAPI) kick(w http.ResponseWriter, r *http.Request) {
	if !a.requireAuth(w, r) {
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	peerID := r.URL.Query().Get("peerID")
	if !validPeerID(peerID) {
		http.Error(w, "invalid node", http.StatusBadRequest)
		return
	}
	if err := a.mesh.kick(peerID); err != nil {
		http.Error(w, err.Error(), http.StatusForbidden)
		return
	}
	w.Write([]byte("removed"))
}

func summarize(direct, relay, connecting int) string {
	total := direct + relay + connecting
	if total == 0 {
		return "waiting for peers to join…"
	}
	s := ""
	if direct > 0 {
		s += itoa(direct) + " direct"
	}
	if relay > 0 {
		if s != "" {
			s += ", "
		}
		s += itoa(relay) + " relayed"
	}
	if connecting > 0 {
		if s != "" {
			s += ", "
		}
		s += itoa(connecting) + " connecting"
	}
	return s
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b []byte
	for n > 0 {
		b = append([]byte{byte('0' + n%10)}, b...)
		n /= 10
	}
	return string(b)
}
