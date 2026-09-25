package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"log"
	"net"
	"sync"
	"time"

	"github.com/pion/ice/v4"
	"github.com/pion/stun/v4"
)

// payload type tags (first plaintext byte inside the AEAD)
const (
	tFrame = 0x00 // an Ethernet frame
	tPing  = 0x01 // keepalive/RTT probe
	tPong  = 0x02
	tMac   = 0x03 // announce my (virtualIP, TAP MAC) so peers can answer ARP locally
)

type PeerState string

const (
	StConnecting PeerState = "connecting"
	StDirect     PeerState = "direct"
	StRelay      PeerState = "relay"
	StFailed     PeerState = "failed"
)

type Mesh struct {
	myID, myName, joinCode, myIP string
	relayMode                    string
	dp                           Datapath
	coord                        *coordClient
	stunURL                      string
	turnURL, turnUser, turnPass  string

	mu       sync.Mutex
	peers    map[string]*Peer
	macTable sync.Map // string(MAC) -> peerID   (learned + announced)
	ipToMac  sync.Map // string(4-byte IP) -> net.HardwareAddr  (announced, for local ARP)
	version  int
	onChange func()
	closed   chan struct{}
}

func newMesh(id, name, code, relayMode string, dp Datapath, coord *coordClient) *Mesh {
	return &Mesh{
		myID: id, myName: name, joinCode: code, relayMode: relayMode, dp: dp, coord: coord,
		stunURL: "stun:stun.cloudflare.com:3478",
		peers:   map[string]*Peer{},
		closed:  make(chan struct{}),
	}
}

func (m *Mesh) Run(name string) error {
	jr, err := m.coord.join(name)
	if err != nil {
		return err
	}
	if m.relayMode != "off" && jr.Turn != nil {
		m.turnURL, m.turnUser, m.turnPass = jr.Turn.URL, jr.Turn.Username, jr.Turn.Credential
	}
	if m.relayMode == "on" && m.turnURL == "" {
		return errors.New("relay mode is on, but the coordinator did not provide relay credentials")
	}
	log.Printf("joined %s as %s (%s); %d member(s)", jr.NetID, name, jr.YourIP, len(jr.Members))
	m.version = jr.Version
	m.reconcile(jr.Members)

	go m.datapathLoop()
	go m.pollLoop()
	return nil
}

func (m *Mesh) Close() {
	select {
	case <-m.closed:
	default:
		close(m.closed)
	}
	m.coord.leave()
	m.mu.Lock()
	for _, p := range m.peers {
		p.stop()
	}
	m.mu.Unlock()
	m.dp.Close()
}

// ---- peer lifecycle ----

func (m *Mesh) ensurePeer(info MemberInfo) *Peer {
	m.mu.Lock()
	defer m.mu.Unlock()
	if info.PeerID == m.myID {
		return nil
	}
	if p, ok := m.peers[info.PeerID]; ok {
		return p
	}
	p := &Peer{
		id: info.PeerID, name: info.Name, ip: info.IP,
		mesh: m, state: StConnecting,
		sigInbox: make(chan iceSignal, 8),
		stopCh:   make(chan struct{}),
		pingSent: make(map[uint32]time.Time),
	}
	m.peers[info.PeerID] = p
	go p.run()
	m.notify()
	return p
}

func (m *Mesh) removePeer(id string) {
	m.mu.Lock()
	p := m.peers[id]
	delete(m.peers, id)
	m.mu.Unlock()
	if p != nil {
		p.stop()
		m.macTable.Range(func(k, v any) bool {
			if v.(string) == id {
				m.macTable.Delete(k)
			}
			return true
		})
		m.notify()
	}
}

func (m *Mesh) reconcile(members []MemberInfo) {
	live := map[string]bool{}
	for _, mi := range members {
		live[mi.PeerID] = true
		m.ensurePeer(mi)
	}
	m.mu.Lock()
	var gone []string
	for id := range m.peers {
		if !live[id] {
			gone = append(gone, id)
		}
	}
	m.mu.Unlock()
	for _, id := range gone {
		m.removePeer(id)
	}
}

// ---- coordinator poll loop ----

func (m *Mesh) pollLoop() {
	for {
		select {
		case <-m.closed:
			return
		default:
		}
		pr, err := m.coord.poll(m.version)
		if err != nil {
			time.Sleep(2 * time.Second)
			continue
		}
		if len(pr.Members) > 0 || pr.Version != m.version {
			m.version = pr.Version
			m.reconcile(pr.Members)
		}
		for _, msg := range pr.Messages {
			var sig iceSignal
			if json.Unmarshal(msg.Data, &sig) != nil {
				continue
			}
			// make sure a peer object exists to receive it
			p := m.ensurePeer(MemberInfo{PeerID: msg.From, Name: sig.Name})
			if p != nil {
				select {
				case p.sigInbox <- sig:
				default:
				}
			}
		}
	}
}

// ---- datapath: OS frames -> peers (the switch) ----

func (m *Mesh) datapathLoop() {
	for {
		frame, err := m.dp.Read()
		if err != nil {
			return
		}
		if len(frame) < 14 {
			continue
		}
		// ARP suppression: if this is a request for a peer whose MAC we already know
		// (announced over the control channel), answer it locally instead of flooding
		// the tunnel. ARP is the dominant LAN broadcast, so this is the big scaling win.
		if reply := m.tryLocalArp(frame); reply != nil {
			m.dp.Write(reply)
			continue
		}
		dst := net.HardwareAddr(frame[0:6])
		if dst[0]&0x01 == 0x01 { // broadcast or multicast -> replicate to all
			m.broadcast(frame)
			continue
		}
		if v, ok := m.macTable.Load(string(dst)); ok {
			m.sendTo(v.(string), frame)
		} else {
			m.broadcast(frame) // unknown unicast: flood, like a learning switch
		}
	}
}

// tryLocalArp returns a forged ARP reply if `frame` is an ARP request whose target IP maps to a
// peer MAC we already know; otherwise nil (caller floods as before — graceful fallback).
func (m *Mesh) tryLocalArp(frame []byte) []byte {
	if len(frame) < 42 || frame[12] != 0x08 || frame[13] != 0x06 { // not ARP
		return nil
	}
	if frame[20] != 0x00 || frame[21] != 0x01 { // not an ARP request (oper != 1)
		return nil
	}
	targetIP := frame[38:42]
	v, ok := m.ipToMac.Load(string(targetIP))
	if !ok {
		return nil // we don't know this peer's MAC yet -> let it flood
	}
	targetMac := v.(net.HardwareAddr)
	reqMac := frame[6:12] // requester's MAC
	reqIP := frame[28:32] // requester's IP

	r := make([]byte, 42)
	copy(r[0:6], reqMac)     // eth dst = requester
	copy(r[6:12], targetMac) // eth src = the peer we're answering for
	r[12], r[13] = 0x08, 0x06
	r[14], r[15] = 0x00, 0x01 // htype ethernet
	r[16], r[17] = 0x08, 0x00 // ptype IPv4
	r[18], r[19] = 6, 4       // hlen, plen
	r[20], r[21] = 0x00, 0x02 // oper = reply
	copy(r[22:28], targetMac) // sender HW = peer
	copy(r[28:32], targetIP)  // sender IP  = peer
	copy(r[32:38], reqMac)    // target HW  = requester
	copy(r[38:42], reqIP)     // target IP  = requester
	return r
}

func (m *Mesh) broadcast(frame []byte) {
	m.mu.Lock()
	peers := make([]*Peer, 0, len(m.peers))
	for _, p := range m.peers {
		peers = append(peers, p)
	}
	m.mu.Unlock()
	for _, p := range peers {
		p.send(tFrame, frame)
	}
}

func (m *Mesh) sendTo(id string, frame []byte) {
	m.mu.Lock()
	p := m.peers[id]
	m.mu.Unlock()
	if p != nil {
		p.send(tFrame, frame)
	}
}

// deliverFromPeer: a decrypted frame arrived from peer id -> learn src MAC, write to OS.
func (m *Mesh) deliverFromPeer(id string, frame []byte) {
	if len(frame) >= 12 {
		src := net.HardwareAddr(frame[6:12])
		m.macTable.Store(string(src), id)
	}
	m.dp.Write(frame)
}

func (m *Mesh) notify() {
	if m.onChange != nil {
		m.onChange()
	}
}

func (m *Mesh) snapshotPeers() []PeerStatus {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]PeerStatus, 0, len(m.peers))
	for _, p := range m.peers {
		out = append(out, PeerStatus{
			Name: p.name, IP: p.ip, State: string(p.state),
			RTTms: float64(p.rtt.Microseconds()) / 1000.0,
		})
	}
	return out
}

// =========================================================================== Peer

type iceSignal struct {
	Type  string   `json:"type"`
	Name  string   `json:"name"`
	Ufrag string   `json:"ufrag"`
	Pwd   string   `json:"pwd"`
	Cands []string `json:"cands"`
}

type Peer struct {
	id, name, ip string
	mesh         *Mesh
	sigInbox     chan iceSignal
	stopCh       chan struct{}

	mu     sync.Mutex
	agent  *ice.Agent
	conn   *ice.Conn
	sealer *sealer
	state  PeerState
	rtt    time.Duration

	pingMu   sync.Mutex
	pingSeq  uint32
	pingSent map[uint32]time.Time
}

func (p *Peer) stop() {
	select {
	case <-p.stopCh:
	default:
		close(p.stopCh)
	}
	p.mu.Lock()
	if p.conn != nil {
		p.conn.Close()
	}
	if p.agent != nil {
		p.agent.Close()
	}
	p.mu.Unlock()
}

// run keeps a connection to this peer alive, retrying on failure.
func (p *Peer) run() {
	for {
		select {
		case <-p.stopCh:
			return
		default:
		}
		if err := p.connectOnce(); err != nil {
			log.Printf("peer %s connect failed: %v (retrying)", p.name, err)
			p.setState(StFailed, 0)
			select {
			case <-p.stopCh:
				return
			case <-time.After(3 * time.Second):
			}
			continue
		}
		p.readLoop() // blocks until the conn dies
		select {
		case <-p.stopCh:
			return
		default:
			p.setState(StConnecting, 0)
			time.Sleep(1 * time.Second)
		}
	}
}

func (p *Peer) connectOnce() error {
	m := p.mesh
	var urls []*stun.URI
	if m.relayMode != "on" {
		u, err := stun.ParseURI(m.stunURL)
		if err == nil {
			urls = append(urls, u)
		}
	}
	if m.relayMode != "off" && m.turnURL != "" {
		u, err := stun.ParseURI(m.turnURL)
		if err == nil {
			u.Username, u.Password = m.turnUser, m.turnPass
			urls = append(urls, u)
		}
	}
	var candidateTypes []ice.CandidateType
	switch m.relayMode {
	case "off":
		candidateTypes = []ice.CandidateType{ice.CandidateTypeHost, ice.CandidateTypeServerReflexive}
	case "on":
		candidateTypes = []ice.CandidateType{ice.CandidateTypeRelay}
	}
	if m.relayMode == "on" && len(urls) == 0 {
		return errors.New("relay credentials unavailable")
	}
	agent, err := ice.NewAgent(&ice.AgentConfig{
		Urls:           urls,
		CandidateTypes: candidateTypes,
		NetworkTypes:   []ice.NetworkType{ice.NetworkTypeUDP4},
	})
	if err != nil {
		return err
	}
	p.mu.Lock()
	p.agent = agent
	p.mu.Unlock()

	done := make(chan struct{})
	var cands []string
	agent.OnCandidate(func(c ice.Candidate) {
		if c == nil {
			close(done)
			return
		}
		cands = append(cands, c.Marshal())
	})
	if err := agent.GatherCandidates(); err != nil {
		return err
	}
	<-done

	uf, pw, _ := agent.GetLocalUserCredentials()
	m.coord.signal(p.id, iceSignal{Type: "ice", Name: m.myName, Ufrag: uf, Pwd: pw, Cands: cands})

	// wait for the peer's ICE params
	var remote iceSignal
	select {
	case remote = <-p.sigInbox:
	case <-time.After(20 * time.Second):
		return errTimeout
	case <-p.stopCh:
		return errStopped
	}
	for _, cs := range remote.Cands {
		if c, err := ice.UnmarshalCandidate(cs); err == nil {
			agent.AddRemoteCandidate(c)
		}
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	var conn *ice.Conn
	if m.myID < p.id { // deterministic role: lower id controls
		conn, err = agent.Dial(ctx, remote.Ufrag, remote.Pwd)
	} else {
		conn, err = agent.Accept(ctx, remote.Ufrag, remote.Pwd)
	}
	if err != nil {
		return err
	}
	sealer, err := newSealer(m.joinCode, m.myID, p.id)
	if err != nil {
		return err
	}
	pair, _ := agent.GetSelectedCandidatePair()
	st := StDirect
	if pair != nil && (pair.Local.Type() == ice.CandidateTypeRelay || pair.Remote.Type() == ice.CandidateTypeRelay) {
		st = StRelay
	}
	p.mu.Lock()
	p.conn = conn
	p.sealer = sealer
	p.mu.Unlock()
	p.setState(st, 0)
	log.Printf("peer %s connected (%s)", p.name, st)
	p.sendMacAnnounce() // tell the peer our (IP, MAC) so it can answer ARP for us locally
	go p.pingLoop()
	return nil
}

// sendMacAnnounce sends our virtual IP + TAP MAC so the peer can resolve ARP for us without
// flooding. Cheap; re-sent periodically from the ping loop in case the first is lost.
func (p *Peer) sendMacAnnounce() {
	ip4 := net.ParseIP(p.mesh.myIP).To4()
	mac := p.mesh.dp.MAC()
	if ip4 == nil || len(mac) != 6 {
		return
	}
	payload := make([]byte, 10)
	copy(payload[0:4], ip4)
	copy(payload[4:10], mac)
	p.send(tMac, payload)
}

func (p *Peer) readLoop() {
	p.mu.Lock()
	conn, sealer := p.conn, p.sealer
	p.mu.Unlock()
	if conn == nil {
		return
	}
	buf := make([]byte, 65535)
	for {
		n, err := conn.Read(buf)
		if err != nil {
			return
		}
		plain, err := sealer.open(buf[:n])
		if err != nil || len(plain) < 1 {
			continue
		}
		switch plain[0] {
		case tFrame:
			p.mesh.deliverFromPeer(p.id, plain[1:])
		case tMac:
			if len(plain) >= 11 { // [type][ip:4][mac:6]
				ip := append([]byte{}, plain[1:5]...)
				mac := net.HardwareAddr(append([]byte{}, plain[5:11]...))
				p.mesh.ipToMac.Store(string(ip), mac)
				p.mesh.macTable.Store(string(mac), p.id)
			}
		case tPing:
			p.send(tPong, plain[1:]) // echo timestamp
		case tPong:
			if len(plain) >= 5 {
				seq := binary.BigEndian.Uint32(plain[1:5])
				p.pingMu.Lock()
				t0, ok := p.pingSent[seq]
				delete(p.pingSent, seq)
				p.pingMu.Unlock()
				if ok {
					p.setState(p.state, time.Since(t0)) // monotonic, accurate on real networks
				}
			}
		}
	}
}

func (p *Peer) send(typ byte, payload []byte) {
	p.mu.Lock()
	conn, sealer := p.conn, p.sealer
	p.mu.Unlock()
	if conn == nil || sealer == nil {
		return
	}
	plain := make([]byte, 1+len(payload))
	plain[0] = typ
	copy(plain[1:], payload)
	conn.Write(sealer.seal(plain))
}

func (p *Peer) pingLoop() {
	t := time.NewTicker(2 * time.Second)
	defer t.Stop()
	ticks := 0
	for {
		select {
		case <-p.stopCh:
			return
		case <-t.C:
			if ticks%6 == 0 { // ~every 12s, re-announce our MAC (idempotent, covers loss)
				p.sendMacAnnounce()
			}
			ticks++
			p.pingMu.Lock()
			seq := p.pingSeq
			p.pingSeq++
			p.pingSent[seq] = time.Now()
			// forget very old unanswered pings so the map can't grow unbounded
			for s, ts := range p.pingSent {
				if time.Since(ts) > 30*time.Second {
					delete(p.pingSent, s)
				}
			}
			p.pingMu.Unlock()
			var b [4]byte
			binary.BigEndian.PutUint32(b[:], seq)
			p.send(tPing, b[:])
		}
	}
}

func (p *Peer) setState(s PeerState, rtt time.Duration) {
	p.mu.Lock()
	changed := p.state != s
	p.state = s
	if rtt > 0 {
		p.rtt = rtt
	}
	p.mu.Unlock()
	if changed {
		p.mesh.notify()
	}
}
