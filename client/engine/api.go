package main

import (
	"encoding/json"
	"net/http"
)

// controlAPI is the tiny local surface the UI polls. Loopback-only; not exposed off-box.
type controlAPI struct {
	mesh   *Mesh
	code   string
	myID   string
	myName string
	myIP   string
}

func (a *controlAPI) serve(addr string) {
	mux := http.NewServeMux()
	mux.HandleFunc("/status", a.status)
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) { w.Write([]byte("ok")) })
	http.ListenAndServe(addr, mux)
}

type statusResp struct {
	MyID    string       `json:"myID"`
	MyName  string       `json:"myName"`
	MyIP    string       `json:"myIP"`
	Code    string       `json:"code"`
	Peers   []PeerStatus `json:"peers"`
	Summary string       `json:"summary"`
}

func (a *controlAPI) status(w http.ResponseWriter, r *http.Request) {
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
	resp := statusResp{
		MyID: a.myID, MyName: a.myName, MyIP: a.myIP, Code: a.code, Peers: peers,
		Summary: summarize(direct, relay, connecting),
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
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
