//go:build linux

package main

import (
	"reflect"
	"testing"
)

func TestLinuxTAPConfigCommands(t *testing.T) {
	got, err := linuxTAPConfigCommands("orbitlan0", "10.69.0.17", "10.69.0.0/24")
	if err != nil {
		t.Fatalf("linuxTAPConfigCommands returned error: %v", err)
	}
	want := [][]string{
		{"link", "set", "dev", "orbitlan0", "mtu", "1400"},
		{"addr", "replace", "10.69.0.17/24", "brd", "+", "dev", "orbitlan0"},
		{"link", "set", "dev", "orbitlan0", "up"},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("commands mismatch\n got: %#v\nwant: %#v", got, want)
	}
}

func TestLinuxTAPConfigRejectsInvalidCoordinatorData(t *testing.T) {
	tests := []struct {
		name   string
		iface  string
		ip     string
		subnet string
	}{
		{name: "empty interface", ip: "10.69.0.2", subnet: "10.69.0.0/24"},
		{name: "invalid address", iface: "orbitlan0", ip: "nope", subnet: "10.69.0.0/24"},
		{name: "IPv6 address", iface: "orbitlan0", ip: "fd00::2", subnet: "fd00::/64"},
		{name: "outside subnet", iface: "orbitlan0", ip: "10.70.0.2", subnet: "10.69.0.0/24"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := linuxTAPConfigCommands(tc.iface, tc.ip, tc.subnet); err == nil {
				t.Fatal("expected validation error")
			}
		})
	}
}
