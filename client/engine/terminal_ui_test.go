package main

import (
	"slices"
	"sort"
	"strings"
	"testing"
	"time"
)

func TestRenderTerminalIncludesNetworkState(t *testing.T) {
	output := renderTerminal(terminalSnapshot{
		Name: "alice", Code: "abc123", IP: "10.69.0.2", Edition: "community",
		RelayMode: "auto", IsHost: true, Connected: true, Started: time.Now(),
		Peers: []PeerStatus{{Name: "bob", IP: "10.69.0.3", State: "direct", RTTms: 12.4}},
	}, []string{"peer bob connected"}, false)
	for _, want := range []string{"ORBITLAN", "ABC123", "10.69.0.2", "HOST", "bob", "DIRECT", "12 ms"} {
		if !strings.Contains(output, want) {
			t.Errorf("dashboard does not contain %q", want)
		}
	}
}

func TestCleanTerminalTextRemovesControlSequences(t *testing.T) {
	got := cleanTerminalText("node\x1b[31m\nname")
	if strings.ContainsAny(got, "\x1b\n\r") {
		t.Fatalf("control character remained in %q", got)
	}
}

func TestTerminalUIMode(t *testing.T) {
	if enabled, err := shouldUseTerminalUI("on"); err != nil || !enabled {
		t.Fatalf("on = %v, %v", enabled, err)
	}
	if enabled, err := shouldUseTerminalUI("off"); err != nil || enabled {
		t.Fatalf("off = %v, %v", enabled, err)
	}
	if _, err := shouldUseTerminalUI("sparkles"); err == nil {
		t.Fatal("invalid TUI mode was accepted")
	}
}

func TestPeerStatusOrderUsesNumericVirtualIP(t *testing.T) {
	peers := []PeerStatus{
		{ID: "c", IP: "10.69.0.100"},
		{ID: "a", IP: "10.69.0.9"},
		{ID: "b", IP: "10.69.0.10"},
	}
	sort.Slice(peers, func(i, j int) bool { return peerStatusLess(peers[i], peers[j]) })
	got := []string{peers[0].IP, peers[1].IP, peers[2].IP}
	want := []string{"10.69.0.9", "10.69.0.10", "10.69.0.100"}
	if !slices.Equal(got, want) {
		t.Fatalf("peer order = %v, want %v", got, want)
	}
}

func TestLatencyColorBands(t *testing.T) {
	tests := []struct {
		rtt  float64
		want string
	}{
		{20, "38;5;114"},
		{80, "38;5;221"},
		{120, "38;5;208"},
		{220, "38;5;203"},
	}
	for _, test := range tests {
		if got := latencyColor(test.rtt); got != test.want {
			t.Errorf("latencyColor(%v) = %q, want %q", test.rtt, got, test.want)
		}
	}
}
