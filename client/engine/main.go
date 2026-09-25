// OrbitLan engine — joins a network, builds a full-mesh of encrypted ICE links to every peer,
// and bridges Ethernet frames between them and a local datapath (TAP adapter, or loopback for
// testing). Exposes a tiny local control API the UI polls.
package main

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"
)

var (
	errTimeout = errors.New("timed out waiting for peer signal")
	errStopped = errors.New("stopped")
)

// PeerStatus is what the control API reports for each peer.
type PeerStatus struct {
	Name  string  `json:"name"`
	IP    string  `json:"ip"`
	State string  `json:"state"`
	RTTms float64 `json:"rttMs"`
}

func main() {
	server := flag.String("server", "", "coordinator base URL (required)")
	relayMode := flag.String("relay", "auto", "relay mode: off | auto | on")
	code := flag.String("code", "", "network join code")
	name := flag.String("name", hostname(), "your display name")
	dpKind := flag.String("datapath", "tap", "datapath: tap | loopback")
	apiAddr := flag.String("api", "127.0.0.1:9099", "local control API address")
	controlToken := flag.String("control-token", "", "local control API authentication token")
	adapter := flag.String("adapter", "OrbitLan", "TAP adapter name")
	idFlag := flag.String("id", "", "override peer id (testing)")
	inject := flag.Bool("selftest-inject", false, "inject broadcast frames (loopback testing)")
	flag.Parse()

	if *server == "" {
		fmt.Println("need -server <coordinator URL>")
		os.Exit(1)
	}
	if *code == "" {
		fmt.Println("need -code <join code>")
		os.Exit(1)
	}
	if *controlToken == "" {
		fmt.Println("need -control-token <random token>")
		os.Exit(1)
	}
	if *relayMode != "off" && *relayMode != "auto" && *relayMode != "on" {
		fmt.Println("invalid -relay value; use off, auto, or on")
		os.Exit(1)
	}

	peerID := *idFlag
	if peerID == "" {
		peerID = loadOrCreateID()
	}

	// datapath
	var dp Datapath
	var lb *loopbackDatapath
	switch *dpKind {
	case "loopback":
		lb = newLoopback(peerID, func(frame []byte) {
			if len(frame) >= 14 {
				log.Printf("RX frame  dst=%02x src=%02x etype=%02x len=%d",
					frame[0:6], frame[6:12], frame[12:14], len(frame))
			}
		})
		dp = lb
	case "tap":
		t, err := openTAP(*adapter)
		if err != nil {
			log.Fatalf("TAP open failed: %v", err)
		}
		dp = t
	default:
		log.Fatalf("unknown datapath %q", *dpKind)
	}

	coord := newCoord(*server, *code, peerID)
	mesh := newMesh(peerID, *name, *code, *relayMode, dp, coord)

	// control API
	api := &controlAPI{
		mesh: mesh, code: *code, myID: peerID, myName: *name,
		token: *controlToken, shutdown: make(chan struct{}),
	}
	go api.serve(*apiAddr)

	// join + run. For TAP we need the assigned IP to configure the adapter.
	jr, err := coord.join(*name)
	if err != nil {
		log.Fatalf("join failed: %v", err)
	}
	api.myIP = jr.YourIP
	mesh.myIP = jr.YourIP
	if *relayMode != "off" && jr.Turn != nil {
		mesh.turnURL, mesh.turnUser, mesh.turnPass = jr.Turn.URL, jr.Turn.Username, jr.Turn.Credential
	}
	if *relayMode == "on" && mesh.turnURL == "" {
		log.Fatal("relay mode is on, but the coordinator did not provide relay credentials")
	}
	if t, ok := dp.(*tapDatapath); ok {
		if err := configureTAP(t.Name(), jr.YourIP, jr.Subnet); err != nil {
			log.Printf("WARN: could not configure adapter IP: %v", err)
		}
	}
	log.Printf("OrbitLan: joined as %s  ip=%s  peers=%d", *name, jr.YourIP, len(jr.Members))
	mesh.version = jr.Version
	mesh.reconcile(jr.Members)
	go mesh.datapathLoop()
	go mesh.pollLoop()

	if *inject && lb != nil {
		go injectLoop(lb)
	}

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	select {
	case <-sig:
	case <-api.shutdown:
	}
	log.Println("shutting down…")
	mesh.Close()
}

func injectLoop(lb *loopbackDatapath) {
	src := lb.MAC()
	for i := 0; ; i++ {
		frame := make([]byte, 42)
		// broadcast dst
		for j := 0; j < 6; j++ {
			frame[j] = 0xFF
		}
		copy(frame[6:12], src)            // src MAC
		frame[12], frame[13] = 0x08, 0x06 // ARP ethertype
		frame[41] = byte(i)               // vary payload
		lb.Inject(frame)
		time.Sleep(2 * time.Second)
	}
}

func hostname() string {
	h, _ := os.Hostname()
	if h == "" {
		return "player"
	}
	return h
}

func idFilePath() string {
	dir, _ := os.UserConfigDir()
	if dir == "" {
		dir = os.TempDir()
	}
	d := filepath.Join(dir, "OrbitLan")
	os.MkdirAll(d, 0o755)
	return filepath.Join(d, "peerid")
}

func loadOrCreateID() string {
	p := idFilePath()
	if b, err := os.ReadFile(p); err == nil && len(b) >= 8 {
		return string(b)
	}
	var buf [8]byte
	rand.Read(buf[:])
	id := hex.EncodeToString(buf[:])
	os.WriteFile(p, []byte(id), 0o644)
	return id
}

// hardware address helper reused by tests
var _ = net.HardwareAddr{}
