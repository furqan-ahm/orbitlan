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
	"runtime"
	"strings"
	"syscall"
	"time"
)

var (
	errTimeout       = errors.New("timed out waiting for peer signal")
	errStopped       = errors.New("stopped")
	stateDirOverride string
	// Release builds override this with -X main.defaultEdition=supporter. Users can still
	// explicitly select either edition with -edition.
	defaultEdition = "community"
)

const defaultCoordinatorURL = "https://orbitlan.furqan-ahm.workers.dev"

// PeerStatus is what the control API reports for each peer.
type PeerStatus struct {
	ID    string  `json:"id"`
	Name  string  `json:"name"`
	IP    string  `json:"ip"`
	State string  `json:"state"`
	RTTms float64 `json:"rttMs"`
}

func main() {
	if len(os.Args) == 2 && strings.EqualFold(os.Args[1], "demo") {
		runTerminalDemo()
		return
	}
	platformArgs, handled, err := handlePlatformCommand(os.Args[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if handled {
		return
	}
	commandArgs, createRoom, err := normalizeCommandArgs(platformArgs)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	os.Args = append([]string{os.Args[0]}, commandArgs...)

	defaultAdapter := "OrbitLan"
	defaultAPI := "127.0.0.1:9099"
	if runtime.GOOS == "linux" {
		defaultAdapter = "orbitlan0"
		defaultAPI = ""
	}

	server := flag.String("server", defaultCoordinatorURL, "coordinator base URL")
	relayMode := flag.String("relay", "auto", "relay mode: off | auto | on")
	edition := flag.String("edition", defaultEdition, "client edition: community | supporter")
	code := flag.String("code", "", "network join code")
	name := flag.String("name", hostname(), "your display name")
	dpKind := flag.String("datapath", "tap", "datapath: tap | loopback")
	apiAddr := flag.String("api", defaultAPI, "local control API address; empty disables it")
	controlSocket := flag.String("control-socket", "", "local Unix control socket (Linux background mode)")
	controlToken := flag.String("control-token", "", "local control API authentication token")
	adapter := flag.String("adapter", defaultAdapter, "TAP adapter name")
	stateDir := flag.String("state-dir", "", "identity directory (default: user config directory)")
	tuiMode := flag.String("tui", "auto", "terminal dashboard: auto | on | off")
	idFlag := flag.String("id", "", "override peer id (testing)")
	inject := flag.Bool("selftest-inject", false, "inject broadcast frames (loopback testing)")
	flag.Usage = func() {
		out := flag.CommandLine.Output()
		fmt.Fprintln(out, "OrbitLan creates an encrypted virtual LAN between your devices.")
		if runtime.GOOS == "linux" {
			fmt.Fprintln(out, "\nCommands:")
			fmt.Fprintln(out, "  sudo orbitlan new [options]                 create a room in the background")
			fmt.Fprintln(out, "  sudo orbitlan join <code> [options]         join in the background")
			fmt.Fprintln(out, "  orbitlan status | nodes | watch             inspect the active room")
			fmt.Fprintln(out, "  orbitlan kick <name|IP|ID>                  remove a node (host only)")
			fmt.Fprintln(out, "  orbitlan leave                              disconnect")
			fmt.Fprintln(out, "  orbitlan logs                               show recent engine messages")
			fmt.Fprintln(out, "  sudo orbitlan run <code> [options]          foreground/debug mode")
			fmt.Fprintln(out, "  orbitlan demo                               dashboard preview; no connection")
		}
		fmt.Fprintln(out, "\nOptions:")
		flag.PrintDefaults()
	}
	flag.Parse()

	*server = strings.TrimRight(strings.TrimSpace(*server), "/")
	// The Windows UI canonicalizes room codes to lowercase. Keep the wire value identical so a
	// Linux node can join rooms created by both current and already-released Windows clients.
	*code = strings.ToLower(strings.TrimSpace(*code))
	*name = strings.TrimSpace(*name)
	if *server == "" {
		fmt.Println("coordinator URL cannot be empty")
		os.Exit(1)
	}
	if *code == "" && flag.NArg() == 1 {
		*code = strings.ToLower(strings.TrimSpace(flag.Arg(0)))
	}
	if createRoom && *code == "" {
		*code = randomRoomCode()
		fmt.Printf("OrbitLan room code: %s\n", *code)
	}
	if *code == "" {
		fmt.Println("need a room code: orbitlan -code ABC123 (or: orbitlan ABC123)")
		os.Exit(1)
	}
	if *name == "" {
		fmt.Println("display name cannot be empty")
		os.Exit(1)
	}
	if *apiAddr != "" && *controlSocket != "" {
		fmt.Println("use either -api or -control-socket, not both")
		os.Exit(1)
	}
	if *apiAddr != "" && *controlToken == "" {
		fmt.Println("-control-token is required when the local control API is enabled")
		os.Exit(1)
	}
	if *relayMode != "off" && *relayMode != "auto" && *relayMode != "on" {
		fmt.Println("invalid -relay value; use off, auto, or on")
		os.Exit(1)
	}
	if *edition != "community" && *edition != "supporter" {
		fmt.Println("invalid -edition value; use community or supporter")
		os.Exit(1)
	}
	useTUI, err := shouldUseTerminalUI(*tuiMode)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	stateDirOverride = strings.TrimSpace(*stateDir)

	peerID := *idFlag
	if peerID == "" {
		peerID = loadOrCreateID()
	}
	memberToken := loadOrCreateMemberToken()

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

	coord := newCoord(*server, *code, peerID, *edition, memberToken)
	mesh := newMesh(peerID, *name, *code, *relayMode, dp, coord)

	// control API
	api := &controlAPI{
		mesh: mesh, code: *code, myID: peerID, myName: *name,
		token: *controlToken, edition: *edition, relayMode: *relayMode,
		started: time.Now(), shutdown: make(chan struct{}),
	}

	// join + run. For TAP we need the assigned IP to configure the adapter.
	jr, err := coord.join(*name)
	if err != nil {
		log.Fatalf("join failed: %v", err)
	}
	api.myIP = jr.YourIP
	mesh.myIP = jr.YourIP
	mesh.setHost(jr.IsHost, jr.HostPeerID)
	if *relayMode != "off" && jr.Turn != nil {
		mesh.turnURL, mesh.turnUser, mesh.turnPass = jr.Turn.URL, jr.Turn.Username, jr.Turn.Credential
	}
	if *relayMode == "on" && mesh.turnURL == "" {
		log.Fatal("relay mode is on, but the coordinator did not provide relay credentials")
	}
	if t, ok := dp.(*tapDatapath); ok {
		if err := configureTAP(t.Name(), jr.YourIP, jr.Subnet); err != nil {
			if runtime.GOOS == "linux" {
				coord.leave()
				dp.Close()
				log.Fatalf("could not configure Linux TAP interface: %v", err)
			}
			log.Printf("WARN: could not configure adapter IP: %v", err)
		}
	}
	if *controlSocket != "" {
		_ = os.Remove(*controlSocket)
		api.trustedLocal = true
		if err := api.start("unix", *controlSocket); err != nil {
			coord.leave()
			dp.Close()
			log.Fatalf("could not start local control socket: %v", err)
		}
		if err := configureControlSocketOwnership(*controlSocket); err != nil {
			api.close()
			coord.leave()
			dp.Close()
			log.Fatalf("could not secure local control socket: %v", err)
		}
		defer os.Remove(*controlSocket)
		defer api.close()
	} else if *apiAddr != "" {
		if err := api.start("tcp", *apiAddr); err != nil {
			coord.leave()
			dp.Close()
			log.Fatalf("could not start local control API: %v", err)
		}
		defer api.close()
	}
	var dashboard *terminalUI
	logOutput, logFlags, logPrefix := log.Writer(), log.Flags(), log.Prefix()
	if useTUI {
		started := time.Now()
		dashboard = newTerminalUI(os.Stdout, func() terminalSnapshot {
			isHost, _ := mesh.hostStatus()
			return terminalSnapshot{
				Name: *name, Code: *code, IP: jr.YourIP, Edition: *edition,
				RelayMode: *relayMode, IsHost: isHost, Connected: true, Started: started,
				Peers: mesh.snapshotPeers(),
			}
		}, true)
		dashboard.AddEvent(fmt.Sprintf("Joined room %s as %s.", strings.ToUpper(*code), *name))
		log.SetFlags(0)
		log.SetPrefix("")
		log.SetOutput(dashboard)
		dashboard.Start()
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
	case err := <-mesh.removed:
		log.Printf("room membership ended: %v", err)
	}
	log.Println("shutting down…")
	mesh.Close()
	if dashboard != nil {
		dashboard.Stop()
		log.SetOutput(logOutput)
		log.SetFlags(logFlags)
		log.SetPrefix(logPrefix)
	}
}

// normalizeCommandArgs adds a friendly Linux/headless command form while retaining complete
// compatibility with the flag-only invocation used by the Windows service:
//
//	orbitlan join ABC123 --name alice
//	orbitlan new --name alice
func normalizeCommandArgs(args []string) ([]string, bool, error) {
	if len(args) == 0 {
		return args, false, nil
	}
	switch strings.ToLower(args[0]) {
	case "join":
		if len(args) < 2 || strings.HasPrefix(args[1], "-") {
			return nil, false, errors.New("usage: orbitlan join <room-code> [options]")
		}
		normalized := []string{"-code", args[1]}
		normalized = append(normalized, args[2:]...)
		return normalized, false, nil
	case "new":
		return args[1:], true, nil
	case "help":
		return []string{"-h"}, false, nil
	default:
		return args, false, nil
	}
}

func randomRoomCode() string {
	const alphabet = "abcdefghjkmnpqrstuvwxyz23456789"
	var random [6]byte
	if _, err := rand.Read(random[:]); err != nil {
		log.Fatalf("could not create room code: %v", err)
	}
	code := make([]byte, len(random))
	for i, value := range random {
		code[i] = alphabet[int(value)%len(alphabet)]
	}
	return string(code)
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
	dir := stateDirOverride
	if dir == "" {
		dir, _ = os.UserConfigDir()
	}
	if dir == "" {
		dir = os.TempDir()
	}
	d := dir
	if stateDirOverride == "" {
		d = filepath.Join(dir, "OrbitLan")
	}
	if err := os.MkdirAll(d, 0o700); err != nil {
		log.Fatalf("could not create identity directory: %v", err)
	}
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
	if err := os.WriteFile(p, []byte(id), 0o600); err != nil {
		log.Fatalf("could not save peer identity: %v", err)
	}
	return id
}

func loadOrCreateMemberToken() string {
	p := filepath.Join(filepath.Dir(idFilePath()), "membertoken")
	if b, err := os.ReadFile(p); err == nil && len(b) == 64 {
		return string(b)
	}
	var buf [32]byte
	if _, err := rand.Read(buf[:]); err != nil {
		log.Fatalf("could not create member identity: %v", err)
	}
	token := hex.EncodeToString(buf[:])
	if err := os.WriteFile(p, []byte(token), 0o600); err != nil {
		log.Fatalf("could not save member identity: %v", err)
	}
	return token
}

// hardware address helper reused by tests
var _ = net.HardwareAddr{}
