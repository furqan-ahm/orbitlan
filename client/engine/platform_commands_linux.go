//go:build linux

package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
)

var (
	linuxRuntimeDir    = envOrDefault("ORBITLAN_RUNTIME_DIR", "/run/orbitlan")
	linuxControlSocket = filepath.Join(linuxRuntimeDir, "control.sock")
	linuxPIDFile       = filepath.Join(linuxRuntimeDir, "orbitlan.pid")
	linuxLogFile       = envOrDefault("ORBITLAN_LOG_FILE", "/var/log/orbitlan.log")
	linuxStateDir      = envOrDefault("ORBITLAN_STATE_DIR", "/var/lib/orbitlan")
)

func envOrDefault(name, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(name)); value != "" {
		return value
	}
	return fallback
}

func handlePlatformCommand(args []string) ([]string, bool, error) {
	if len(args) == 0 {
		return args, false, nil
	}
	switch strings.ToLower(args[0]) {
	case "join":
		if len(args) < 2 || strings.HasPrefix(args[1], "-") {
			return nil, true, errors.New("usage: orbitlan join <room-code> [--name NAME] [options]")
		}
		code := strings.ToLower(strings.TrimSpace(args[1]))
		return nil, true, startLinuxBackground(code, args[2:])
	case "new":
		code := randomRoomCode()
		if err := startLinuxBackground(code, args[1:]); err != nil {
			return nil, true, err
		}
		fmt.Printf("Room created. Share this code: %s\n", strings.ToUpper(code))
		return nil, true, nil
	case "run":
		if len(args) < 2 || strings.HasPrefix(args[1], "-") {
			return nil, true, errors.New("usage: orbitlan run <room-code> [--name NAME] [options]")
		}
		foreground := []string{"-code", args[1]}
		return append(foreground, args[2:]...), false, nil
	case "status":
		return nil, true, printLinuxStatus(false)
	case "nodes":
		return nil, true, printLinuxNodes()
	case "watch":
		return nil, true, watchLinuxStatus()
	case "kick":
		if len(args) != 2 {
			return nil, true, errors.New("usage: orbitlan kick <node-name|virtual-IP|node-ID>")
		}
		return nil, true, kickLinuxNode(args[1])
	case "leave", "disconnect":
		return nil, true, stopLinuxBackground()
	case "logs":
		return nil, true, printLinuxLogs()
	default:
		return args, false, nil
	}
}

func requireLinuxRoot(action string) error {
	if os.Geteuid() != 0 {
		return fmt.Errorf("%s needs network-administrator access; run it with sudo", action)
	}
	return nil
}

func startLinuxBackground(code string, options []string) error {
	if err := requireLinuxRoot("connecting"); err != nil {
		return err
	}
	if code == "" {
		return errors.New("room code cannot be empty")
	}
	if status, err := fetchLinuxStatus(); err == nil {
		return fmt.Errorf("already connected to room %s as %s; run 'sudo orbitlan leave' first",
			strings.ToUpper(status.Code), status.MyName)
	}
	if err := os.MkdirAll(linuxRuntimeDir, 0o700); err != nil {
		return fmt.Errorf("create runtime directory: %w", err)
	}
	if err := configureLinuxRuntimeOwnership(); err != nil {
		return err
	}
	if pid, err := readLinuxPID(); err == nil && processAlive(pid) {
		return fmt.Errorf("OrbitLan is already starting (process %d)", pid)
	}
	_ = os.Remove(linuxControlSocket)
	_ = os.Remove(linuxPIDFile)
	if err := os.MkdirAll(linuxStateDir, 0o700); err != nil {
		return fmt.Errorf("create identity directory: %w", err)
	}

	executable, err := os.Executable()
	if err != nil {
		return fmt.Errorf("locate OrbitLan executable: %w", err)
	}
	childArgs := []string{"-code", code}
	childArgs = append(childArgs, options...)
	childArgs = append(childArgs,
		"-api", "", "-control-socket", linuxControlSocket,
		"-tui", "off", "-state-dir", linuxStateDir)

	logFile, err := os.OpenFile(linuxLogFile, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return fmt.Errorf("open %s: %w", linuxLogFile, err)
	}
	defer logFile.Close()
	if uid, gid, ok := sudoOwner(); ok {
		if err := logFile.Chown(uid, gid); err != nil {
			return fmt.Errorf("set background log owner: %w", err)
		}
	}
	cmd := exec.Command(executable, childArgs...)
	cmd.Stdin = nil
	cmd.Stdout = logFile
	cmd.Stderr = logFile
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("start OrbitLan: %w", err)
	}
	pid := cmd.Process.Pid
	if err := os.WriteFile(linuxPIDFile, []byte(strconv.Itoa(pid)), 0o600); err != nil {
		_ = cmd.Process.Kill()
		return fmt.Errorf("record background process: %w", err)
	}
	_ = cmd.Process.Release()

	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := fetchLinuxStatus(); err == nil {
			status, _ := fetchLinuxStatus()
			fmt.Printf("Connected to %s as %s (%s).\n", strings.ToUpper(status.Code), status.MyName, status.MyIP)
			fmt.Println("Use 'orbitlan status' or 'orbitlan watch'; the connection stays in the background.")
			return nil
		}
		if !processAlive(pid) {
			return fmt.Errorf("connection failed; check %s", linuxLogFile)
		}
		time.Sleep(200 * time.Millisecond)
	}
	return fmt.Errorf("connection is still starting; check 'orbitlan status' or %s", linuxLogFile)
}

func linuxHTTPClient() *http.Client {
	transport := &http.Transport{
		DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
			return (&net.Dialer{Timeout: 2 * time.Second}).DialContext(ctx, "unix", linuxControlSocket)
		},
	}
	return &http.Client{Transport: transport, Timeout: 3 * time.Second}
}

func sudoOwner() (int, int, bool) {
	uid, uidErr := strconv.Atoi(os.Getenv("SUDO_UID"))
	gid, gidErr := strconv.Atoi(os.Getenv("SUDO_GID"))
	return uid, gid, uidErr == nil && gidErr == nil && uid >= 0 && gid >= 0
}

func configureLinuxRuntimeOwnership() error {
	uid, gid, ok := sudoOwner()
	if !ok {
		return nil
	}
	if err := os.Chown(linuxRuntimeDir, uid, gid); err != nil {
		return fmt.Errorf("set control directory owner: %w", err)
	}
	if err := os.Chmod(linuxRuntimeDir, 0o700); err != nil {
		return fmt.Errorf("secure control directory: %w", err)
	}
	return nil
}

func configureControlSocketOwnership(path string) error {
	uid, gid, ok := sudoOwner()
	if !ok {
		return nil
	}
	if err := os.Chown(path, uid, gid); err != nil {
		return fmt.Errorf("set control socket owner: %w", err)
	}
	return os.Chmod(path, 0o600)
}

func linuxControl(method, path string, result any) error {
	req, err := http.NewRequest(method, "http://orbitlan.local"+path, nil)
	if err != nil {
		return err
	}
	resp, err := linuxHTTPClient().Do(req)
	if err != nil {
		if errors.Is(err, os.ErrPermission) || os.IsPermission(err) {
			return errors.New("cannot access the OrbitLan control socket; retry with sudo")
		}
		return errors.New("OrbitLan is not connected; use 'sudo orbitlan join <room-code>'")
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		message, _ := io.ReadAll(io.LimitReader(resp.Body, 1024))
		return fmt.Errorf("OrbitLan: %s", strings.TrimSpace(string(message)))
	}
	if result != nil {
		if err := json.NewDecoder(resp.Body).Decode(result); err != nil {
			return err
		}
	}
	return nil
}

func fetchLinuxStatus() (statusResp, error) {
	var status statusResp
	err := linuxControl(http.MethodGet, "/status", &status)
	return status, err
}

func printLinuxStatus(_ bool) error {
	status, err := fetchLinuxStatus()
	if err != nil {
		return err
	}
	started := time.Unix(status.StartedAt, 0)
	fmt.Print(renderTerminal(terminalSnapshot{
		Name: status.MyName, Code: status.Code, IP: status.MyIP, Edition: status.Edition,
		RelayMode: status.RelayMode, IsHost: status.IsHost, Connected: true,
		Started: started, Peers: status.Peers, HideFooter: true,
	}, nil, false))
	fmt.Println()
	return nil
}

func printLinuxNodes() error {
	status, err := fetchLinuxStatus()
	if err != nil {
		return err
	}
	peers := append([]PeerStatus(nil), status.Peers...)
	sort.Slice(peers, func(i, j int) bool { return strings.ToLower(peers[i].Name) < strings.ToLower(peers[j].Name) })
	if len(peers) == 0 {
		fmt.Println("No other nodes are in this room yet.")
		return nil
	}
	fmt.Printf("%-22s %-15s %-12s %-8s %s\n", "NAME", "VIRTUAL IP", "PATH", "LATENCY", "NODE ID")
	for _, peer := range peers {
		latency := "—"
		if peer.RTTms > 0 {
			latency = fmt.Sprintf("%.0fms", peer.RTTms)
		}
		fmt.Printf("%-22s %-15s %-12s %-8s %s\n", fitText(cleanTerminalText(peer.Name), 22),
			peer.IP, strings.ToUpper(peer.State), latency, peer.ID)
	}
	return nil
}

func watchLinuxStatus() error {
	if !stdoutIsTerminal() {
		return errors.New("'orbitlan watch' needs an interactive terminal; use 'orbitlan status' in scripts")
	}
	initial, err := fetchLinuxStatus()
	if err != nil {
		return err
	}
	last := initial
	connected := true
	ui := newTerminalUI(os.Stdout, func() terminalSnapshot {
		if current, fetchErr := fetchLinuxStatus(); fetchErr == nil {
			last = current
			connected = true
		} else {
			connected = false
		}
		return terminalSnapshot{
			Name: last.MyName, Code: last.Code, IP: last.MyIP, Edition: last.Edition,
			RelayMode: last.RelayMode, IsHost: last.IsHost, Connected: connected,
			Started: time.Unix(last.StartedAt, 0), Peers: last.Peers, DetachOnly: true,
		}
	}, true)
	ui.AddEvent("Attached to the background connection.")
	ui.Start()
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	<-sig
	signal.Stop(sig)
	ui.Stop()
	return nil
}

func kickLinuxNode(target string) error {
	status, err := fetchLinuxStatus()
	if err != nil {
		return err
	}
	if !status.IsHost {
		return errors.New("only the room host can kick nodes")
	}
	needle := strings.ToLower(strings.TrimSpace(target))
	var matches []PeerStatus
	for _, peer := range status.Peers {
		if strings.EqualFold(peer.ID, needle) || strings.EqualFold(peer.Name, needle) || peer.IP == target ||
			strings.HasPrefix(strings.ToLower(peer.ID), needle) {
			matches = append(matches, peer)
		}
	}
	if len(matches) == 0 {
		return fmt.Errorf("node %q was not found; use 'orbitlan nodes' to list valid targets", target)
	}
	if len(matches) > 1 {
		return fmt.Errorf("node %q is ambiguous; use its virtual IP or full node ID", target)
	}
	path := "/kick?peerID=" + url.QueryEscape(matches[0].ID)
	if err := linuxControl(http.MethodPost, path, nil); err != nil {
		return err
	}
	fmt.Printf("Removed %s (%s) from the room.\n", matches[0].Name, matches[0].IP)
	return nil
}

func stopLinuxBackground() error {
	if err := linuxControl(http.MethodPost, "/shutdown", nil); err != nil {
		return err
	}
	for i := 0; i < 30; i++ {
		if _, err := os.Stat(linuxControlSocket); errors.Is(err, os.ErrNotExist) {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	_ = os.Remove(linuxPIDFile)
	fmt.Println("Disconnected from OrbitLan.")
	return nil
}

func readLinuxPID() (int, error) {
	data, err := os.ReadFile(linuxPIDFile)
	if err != nil {
		return 0, err
	}
	return strconv.Atoi(strings.TrimSpace(string(data)))
}

func processAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	err := syscall.Kill(pid, 0)
	return err == nil || errors.Is(err, syscall.EPERM)
}

func printLinuxLogs() error {
	file, err := os.Open(filepath.Clean(linuxLogFile))
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return errors.New("no OrbitLan background log exists yet")
		}
		return err
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	var lines []string
	for scanner.Scan() {
		lines = append(lines, scanner.Text())
		if len(lines) > 40 {
			lines = lines[1:]
		}
	}
	for _, line := range lines {
		fmt.Println(line)
	}
	return scanner.Err()
}
