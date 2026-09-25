//go:build !windows

package main

import "fmt"

// Non-Windows stub so the module builds everywhere; the real TAP lives in tap_windows.go.
type tapDatapath struct{ Datapath }

func (t *tapDatapath) Name() string { return "" }

func openTAP(name string) (*tapDatapath, error) {
	return nil, fmt.Errorf("TAP datapath is Windows-only; use -datapath loopback here")
}
func configureTAP(name, ip, subnetCIDR string) error { return nil }
