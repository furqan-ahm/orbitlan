package main

import (
	"fmt"
	"io"
	"math"
	"os"
	"os/signal"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unicode"
)

const dashboardWidth = 76

type terminalSnapshot struct {
	Name       string
	Code       string
	IP         string
	Edition    string
	RelayMode  string
	IsHost     bool
	Connected  bool
	DetachOnly bool
	Preview    bool
	HideFooter bool
	Started    time.Time
	Peers      []PeerStatus
}

// terminalUI is deliberately small: it uses ANSI screen controls instead of a TUI framework,
// redraws once per second, and remains completely idle between updates.
type terminalUI struct {
	out      io.Writer
	snapshot func() terminalSnapshot
	color    bool

	mu     sync.Mutex
	events []string
	stop   chan struct{}
	done   chan struct{}
	once   sync.Once
}

func newTerminalUI(out io.Writer, snapshot func() terminalSnapshot, color bool) *terminalUI {
	return &terminalUI{
		out: out, snapshot: snapshot, color: color,
		stop: make(chan struct{}), done: make(chan struct{}),
	}
}

func (u *terminalUI) Start() {
	fmt.Fprint(u.out, "\x1b[?1049h\x1b[?25l\x1b[2J")
	u.draw()
	go func() {
		defer close(u.done)
		ticker := time.NewTicker(time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				u.draw()
			case <-u.stop:
				return
			}
		}
	}()
}

func (u *terminalUI) Stop() {
	u.once.Do(func() { close(u.stop) })
	<-u.done
	fmt.Fprint(u.out, "\x1b[?25h\x1b[?1049l")
}

func (u *terminalUI) draw() {
	u.mu.Lock()
	events := append([]string(nil), u.events...)
	u.mu.Unlock()
	fmt.Fprint(u.out, "\x1b[H", renderTerminal(u.snapshot(), events, u.color))
}

// Write makes terminalUI a log.Writer. Network messages become short dashboard events instead
// of scrolling the terminal and corrupting the screen.
func (u *terminalUI) Write(p []byte) (int, error) {
	for _, line := range strings.Split(string(p), "\n") {
		if clean := cleanTerminalText(line); clean != "" {
			u.AddEvent(clean)
		}
	}
	return len(p), nil
}

func (u *terminalUI) AddEvent(message string) {
	message = cleanTerminalText(message)
	if message == "" {
		return
	}
	u.mu.Lock()
	u.events = append(u.events, message)
	if len(u.events) > 12 {
		u.events = u.events[len(u.events)-12:]
	}
	u.mu.Unlock()
}

func renderTerminal(s terminalSnapshot, events []string, color bool) string {
	peers := append([]PeerStatus(nil), s.Peers...)
	sort.Slice(peers, func(i, j int) bool { return peerStatusLess(peers[i], peers[j]) })

	accent := func(code, text string) string {
		if !color {
			return text
		}
		return "\x1b[" + code + "m" + text + "\x1b[0m"
	}
	status := accent("38;5;114", "● LIVE")
	if !s.Connected {
		status = accent("38;5;203", "● OFFLINE")
		if s.Preview {
			status = accent("38;5;221", "● DEMO")
		}
	}
	role := "MEMBER"
	if s.IsHost {
		role = "HOST"
	}
	edition := strings.ToUpper(s.Edition)
	if edition == "" {
		edition = "COMMUNITY"
	}
	code := strings.ToUpper(cleanTerminalText(s.Code))
	name := cleanTerminalText(s.Name)
	ip := cleanTerminalText(s.IP)
	if ip == "" {
		ip = "waiting…"
	}
	uptime := time.Since(s.Started).Truncate(time.Second)
	if uptime < 0 || s.Started.IsZero() {
		uptime = 0
	}

	var b strings.Builder
	b.WriteString(accent("38;5;141", "  ◉ ORBITLAN"))
	b.WriteString("  //  PRIVATE LAN FOR LINUX")
	b.WriteString(strings.Repeat(" ", max(1, dashboardWidth-42-visibleLen(status))))
	b.WriteString(status)
	b.WriteString("\n")
	b.WriteString(accent("38;5;60", "  "+strings.Repeat("─", dashboardWidth-4)))
	b.WriteString("\n\n")
	b.WriteString("  ")
	b.WriteString(accent("1;97", code))
	b.WriteString("  ")
	b.WriteString(accent("38;5;245", "ROOM CODE"))
	b.WriteString("       ")
	b.WriteString(accent("1;97", ip))
	b.WriteString("  ")
	b.WriteString(accent("38;5;245", "VIRTUAL IP"))
	b.WriteString("\n")
	b.WriteString(fmt.Sprintf("  %-20s  %-10s  relay %-5s  up %s\n",
		fitText(name, 20), role, strings.ToUpper(s.RelayMode), formatUptime(uptime)))
	b.WriteString("\n")
	b.WriteString(accent("1;97", fmt.Sprintf("  NODES  %d connected", connectedCount(peers))))
	b.WriteString("\n")
	b.WriteString(accent("38;5;60", "  "+strings.Repeat("─", dashboardWidth-4)))
	b.WriteString("\n")
	b.WriteString(accent("38;5;245", "  NAME                     ADDRESS          PATH          LATENCY"))
	b.WriteString("\n")
	if len(peers) == 0 {
		b.WriteString(accent("38;5;245", "  Waiting for another node to join…"))
		b.WriteString("\n")
	} else {
		maxRows := terminalRows() - 16
		if maxRows < 2 {
			maxRows = 2
		}
		if maxRows > 8 {
			maxRows = 8
		}
		shown := min(len(peers), maxRows)
		for _, peer := range peers[:shown] {
			state, stateColor := displayPeerState(peer.State)
			latency := "—"
			if peer.RTTms > 0 {
				latency = fmt.Sprintf("%.0f ms", peer.RTTms)
			}
			marker := accent(stateColor, "●")
			b.WriteString(fmt.Sprintf("  %s %-22s  %-15s  %-12s  %s\n", marker,
				fitText(cleanTerminalText(peer.Name), 22), fitText(cleanTerminalText(peer.IP), 15),
				accent(stateColor, fitText(state, 12)), accent(latencyColor(peer.RTTms), latency)))
		}
		if len(peers) > shown {
			b.WriteString(accent("38;5;245", fmt.Sprintf("  … and %d more node(s)", len(peers)-shown)))
			b.WriteString("\n")
		}
	}

	b.WriteString("\n")
	b.WriteString(accent("1;97", "  RECENT ACTIVITY"))
	b.WriteString("\n")
	start := max(0, len(events)-3)
	if start == len(events) {
		b.WriteString(accent("38;5;245", "  Network is ready."))
		b.WriteString("\n")
	} else {
		for _, event := range events[start:] {
			b.WriteString(accent("38;5;245", "  · "+fitText(event, dashboardWidth-8)))
			b.WriteString("\n")
		}
	}
	if !s.HideFooter {
		b.WriteString("\n")
		footer := "  Ctrl+C disconnects safely"
		if s.DetachOnly {
			footer = "  Ctrl+C closes this view · the connection stays active"
		}
		b.WriteString(accent("38;5;245", footer))
	}
	if color {
		b.WriteString("\x1b[J")
	}
	return b.String()
}

func latencyColor(rttMS float64) string {
	switch {
	case rttMS <= 0:
		return "38;5;245"
	case rttMS < 60:
		return "38;5;114"
	case rttMS < 100:
		return "38;5;221"
	case rttMS < 180:
		return "38;5;208"
	default:
		return "38;5;203"
	}
}

func displayPeerState(state string) (string, string) {
	switch strings.ToLower(state) {
	case "direct":
		return "DIRECT", "38;5;114"
	case "relay":
		return "RELAY", "38;5;221"
	case "failed":
		return "RETRYING", "38;5;203"
	default:
		return "CONNECTING", "38;5;147"
	}
}

func connectedCount(peers []PeerStatus) int {
	count := 0
	for _, peer := range peers {
		if peer.State == "direct" || peer.State == "relay" {
			count++
		}
	}
	return count
}

func cleanTerminalText(value string) string {
	value = strings.TrimSpace(value)
	var b strings.Builder
	for _, r := range value {
		if unicode.IsControl(r) {
			continue
		}
		b.WriteRune(r)
	}
	return b.String()
}

func fitText(value string, width int) string {
	runes := []rune(value)
	if len(runes) > width {
		if width <= 1 {
			return string(runes[:width])
		}
		return string(runes[:width-1]) + "…"
	}
	return value + strings.Repeat(" ", width-len(runes))
}

func visibleLen(value string) int {
	insideEscape := false
	length := 0
	for _, r := range value {
		if r == '\x1b' {
			insideEscape = true
			continue
		}
		if insideEscape {
			if unicode.IsLetter(r) {
				insideEscape = false
			}
			continue
		}
		length++
	}
	return length
}

func terminalRows() int {
	if rows, err := strconv.Atoi(os.Getenv("LINES")); err == nil && rows > 0 {
		return rows
	}
	return 24
}

func formatUptime(d time.Duration) string {
	seconds := int(d.Seconds())
	if seconds < 3600 {
		return fmt.Sprintf("%02d:%02d", seconds/60, seconds%60)
	}
	return fmt.Sprintf("%02d:%02d:%02d", seconds/3600, (seconds/60)%60, seconds%60)
}

func stdoutIsTerminal() bool {
	info, err := os.Stdout.Stat()
	return err == nil && info.Mode()&os.ModeCharDevice != 0 && os.Getenv("TERM") != "dumb"
}

func shouldUseTerminalUI(mode string) (bool, error) {
	switch strings.ToLower(strings.TrimSpace(mode)) {
	case "auto":
		return stdoutIsTerminal(), nil
	case "on":
		return true, nil
	case "off":
		return false, nil
	default:
		return false, fmt.Errorf("invalid -tui value %q; use auto, on, or off", mode)
	}
}

func runTerminalDemo() {
	started := time.Now()
	snapshot := func() terminalSnapshot {
		phase := time.Since(started).Seconds() / 3
		peers := []PeerStatus{
			{Name: "gaming-pc", IP: "10.69.0.3", State: "direct", RTTms: 18 + 4*math.Sin(phase)},
			{Name: "linux-server", IP: "10.69.0.4", State: "direct", RTTms: 31 + 5*math.Cos(phase)},
			{Name: "old-laptop", IP: "10.69.0.5", State: "relay", RTTms: 76 + 8*math.Sin(phase)},
		}
		return terminalSnapshot{
			Name: "wsl-preview", Code: "ORBIT7", IP: "10.69.0.2", Edition: "community",
			RelayMode: "auto", IsHost: true, Connected: false, Preview: true,
			Started: started, Peers: peers,
		}
	}
	if !stdoutIsTerminal() {
		fmt.Print(renderTerminal(snapshot(), []string{"Demo mode: no network connection was made."}, false))
		return
	}
	u := newTerminalUI(os.Stdout, snapshot, true)
	u.AddEvent("Demo mode: no network connection was made.")
	u.Start()
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(sig)
	<-sig
	u.Stop()
}
