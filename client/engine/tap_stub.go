//go:build !windows && !linux

package main

import "fmt"

// Unsupported-platform stub so the module still builds on platforms where OrbitLan does not
// yet have an OS-facing TAP implementation. Windows and Linux have real implementations.
type tapDatapath struct{ Datapath }

func (t *tapDatapath) Name() string { return "" }

func openTAP(name string) (*tapDatapath, error) {
	return nil, fmt.Errorf("TAP datapath is not supported on this operating system; use -datapath loopback for testing")
}
func configureTAP(name, ip, subnetCIDR string) error { return nil }
