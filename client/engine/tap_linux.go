//go:build linux

package main

import (
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"strings"
	"sync"

	"golang.org/x/sys/unix"
)

// Linux exposes layer-2 virtual interfaces through /dev/net/tun. IFF_TAP gives us complete
// Ethernet frames and IFF_NO_PI omits the four-byte kernel packet-information prefix, matching
// the frame format used by the Windows TAP-Windows6 datapath.
type tapDatapath struct {
	file      *os.File
	name      string
	mac       net.HardwareAddr
	writeMu   sync.Mutex
	closeOnce sync.Once
}

func (t *tapDatapath) Name() string { return t.name }

func openTAP(name string) (*tapDatapath, error) {
	if strings.TrimSpace(name) == "" {
		return nil, errors.New("Linux TAP interface name cannot be empty")
	}

	file, err := os.OpenFile("/dev/net/tun", os.O_RDWR|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, fmt.Errorf("open /dev/net/tun: %w (run as root or grant CAP_NET_ADMIN)", err)
	}

	ifr, err := unix.NewIfreq(name)
	if err != nil {
		file.Close()
		return nil, fmt.Errorf("invalid TAP interface name %q: %w", name, err)
	}
	ifr.SetUint16(unix.IFF_TAP | unix.IFF_NO_PI)
	if err := unix.IoctlIfreq(int(file.Fd()), unix.TUNSETIFF, ifr); err != nil {
		file.Close()
		return nil, fmt.Errorf("create TAP interface %q: %w (run as root or grant CAP_NET_ADMIN)", name, err)
	}

	// Do not cache the MAC while the new interface is still being initialized. On Ubuntu,
	// systemd-networkd can apply MACAddressPolicy=persistent when the link is brought up, which
	// replaces the creation-time address. MAC() is first used after configureTAP has brought the
	// interface up, so looking it up lazily announces the address the kernel will actually use.
	return &tapDatapath{file: file, name: ifr.Name()}, nil
}

func (t *tapDatapath) Read() ([]byte, error) {
	// MTU is 1400; 2048 leaves room for the Ethernet header and VLAN tags without allocating a
	// jumbo-frame-sized buffer for every game packet. Linux returns one TAP frame per read.
	// Use the Linux syscall directly: Go's os.File poller can classify /dev/net/tun as
	// non-pollable on some Oracle kernels and return "not pollable" instead of blocking.
	frame := make([]byte, 2048)
	for {
		n, err := unix.Read(int(t.file.Fd()), frame)
		if err == unix.EINTR {
			continue
		}
		if err != nil {
			return nil, err
		}
		return frame[:n], nil
	}
}

func (t *tapDatapath) Write(frame []byte) error {
	t.writeMu.Lock()
	defer t.writeMu.Unlock()
	for {
		n, err := unix.Write(int(t.file.Fd()), frame)
		if err == unix.EINTR {
			continue
		}
		if err != nil {
			return err
		}
		if n != len(frame) {
			return io.ErrShortWrite
		}
		return nil
	}
}

func (t *tapDatapath) MAC() net.HardwareAddr {
	if len(t.mac) == 6 {
		return append(net.HardwareAddr(nil), t.mac...)
	}
	if iface, err := net.InterfaceByName(t.name); err == nil && len(iface.HardwareAddr) == 6 {
		t.mac = append(net.HardwareAddr(nil), iface.HardwareAddr...)
		return append(net.HardwareAddr(nil), t.mac...)
	}
	return macFromID("linux-tap:" + t.name)
}

func (t *tapDatapath) Close() error {
	var err error
	t.closeOnce.Do(func() { err = t.file.Close() })
	return err
}

// linuxTAPConfigCommands validates the coordinator response and produces argument arrays rather
// than shell text. Besides being testable, this prevents interface names or server responses
// from being interpreted by a shell.
func linuxTAPConfigCommands(name, ip, subnetCIDR string) ([][]string, error) {
	if strings.TrimSpace(name) == "" {
		return nil, errors.New("TAP interface name cannot be empty")
	}
	parsedIP := net.ParseIP(ip).To4()
	if parsedIP == nil {
		return nil, fmt.Errorf("invalid assigned IPv4 address %q", ip)
	}
	_, network, err := net.ParseCIDR(subnetCIDR)
	if err != nil {
		return nil, fmt.Errorf("invalid coordinator subnet %q: %w", subnetCIDR, err)
	}
	ones, bits := network.Mask.Size()
	if bits != 32 || !network.Contains(parsedIP) {
		return nil, fmt.Errorf("assigned address %s is outside IPv4 subnet %s", ip, subnetCIDR)
	}

	address := parsedIP.String() + fmt.Sprintf("/%d", ones)
	return [][]string{
		{"link", "set", "dev", name, "mtu", "1400"},
		{"addr", "replace", address, "brd", "+", "dev", name},
		{"link", "set", "dev", name, "up"},
	}, nil
}

// configureTAP assigns the coordinator-provided address and brings the ephemeral interface up.
// The connected subnet route is installed automatically by `ip addr replace`.
func configureTAP(name, ip, subnetCIDR string) error {
	commands, err := linuxTAPConfigCommands(name, ip, subnetCIDR)
	if err != nil {
		return err
	}
	if _, err := exec.LookPath("ip"); err != nil {
		return errors.New("the ip command is required (install the iproute2 package)")
	}
	for _, args := range commands {
		if output, err := exec.Command("ip", args...).CombinedOutput(); err != nil {
			return fmt.Errorf("ip %s: %w (%s)", strings.Join(args, " "), err, strings.TrimSpace(string(output)))
		}
	}
	return nil
}
