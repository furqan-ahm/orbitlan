//go:build windows

package main

import (
	"fmt"
	"net"
	"os/exec"
	"strings"
	"sync"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

// tap-windows6 constants (verified against OpenVPN tap-windows.h).
const (
	tapSetMediaStatus = 0x00220018 // TAP_WIN_IOCTL_SET_MEDIA_STATUS
	tapGetMAC         = 0x00220004 // TAP_WIN_IOCTL_GET_MAC
	adapterClassKey   = `SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}`
	networkConnKey    = `SYSTEM\CurrentControlSet\Control\Network\{4D36E972-E325-11CE-BFC1-08002BE10318}`
	tapComponentID    = "tap0901"
)

type tapDatapath struct {
	handle windows.Handle
	name   string // the adapter's connection name (for netsh)
	mac    net.HardwareAddr
	rOvl   *windows.Overlapped
	wOvl   *windows.Overlapped
	wmu    sync.Mutex
	closed bool
}

func (t *tapDatapath) Name() string { return t.name }

// findAdapterGUID returns the NetCfgInstanceId of the tap-windows6 adapter whose connection
// name matches `wantName` (or the first tap0901 adapter if wantName is empty).
func findAdapterGUID(wantName string) (string, error) {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, adapterClassKey, registry.READ)
	if err != nil {
		return "", err
	}
	defer k.Close()
	subs, err := k.ReadSubKeyNames(-1)
	if err != nil {
		return "", err
	}
	var firstTap string
	for _, s := range subs {
		sk, err := registry.OpenKey(registry.LOCAL_MACHINE, adapterClassKey+`\`+s, registry.READ)
		if err != nil {
			continue
		}
		cid, _, _ := sk.GetStringValue("ComponentId")
		guid, _, _ := sk.GetStringValue("NetCfgInstanceId")
		sk.Close()
		if !strings.EqualFold(cid, tapComponentID) || guid == "" {
			continue
		}
		if firstTap == "" {
			firstTap = guid
		}
		if wantName == "" {
			return guid, nil
		}
		if strings.EqualFold(guid, wantName) {
			return guid, nil
		}
		if strings.EqualFold(connectionName(guid), wantName) {
			return guid, nil
		}
	}
	if wantName == "" && firstTap != "" {
		return firstTap, nil
	}
	return "", fmt.Errorf("no OrbitLan tap-windows6 (%s) adapter found — reinstall OrbitLan to repair it", tapComponentID)
}

func connectionName(guid string) string {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, networkConnKey+`\`+guid+`\Connection`, registry.READ)
	if err != nil {
		return ""
	}
	defer k.Close()
	name, _, _ := k.GetStringValue("Name")
	return name
}

func openTAP(name string) (*tapDatapath, error) {
	guid, err := findAdapterGUID(name)
	if err != nil {
		return nil, err
	}
	path := `\\.\Global\` + guid + `.tap`
	p16, _ := windows.UTF16PtrFromString(path)
	h, err := windows.CreateFile(
		p16,
		windows.GENERIC_READ|windows.GENERIC_WRITE,
		0, nil,
		windows.OPEN_EXISTING,
		windows.FILE_ATTRIBUTE_SYSTEM|windows.FILE_FLAG_OVERLAPPED,
		0,
	)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w (need admin?)", path, err)
	}

	t := &tapDatapath{handle: h, name: connectionName(guid)}
	if t.name == "" {
		t.name = name
	}
	// rename the Windows connection to "OrbitLan" so it's recognizable (default is
	// "TAP-Windows Adapter V9"). Harmless if it already has the name.
	if t.name != "" && !strings.EqualFold(t.name, "OrbitLan") {
		if err := exec.Command("netsh", "interface", "set", "interface",
			"name="+t.name, "newname=OrbitLan").Run(); err == nil {
			t.name = "OrbitLan"
		}
	}

	// bring the adapter "connected"
	status := []byte{1, 0, 0, 0}
	var ret uint32
	if err := windows.DeviceIoControl(h, tapSetMediaStatus,
		&status[0], 4, &status[0], 4, &ret, nil); err != nil {
		windows.CloseHandle(h)
		return nil, fmt.Errorf("set media status: %w", err)
	}

	// read the adapter MAC
	mac := make([]byte, 6)
	if err := windows.DeviceIoControl(h, tapGetMAC, &mac[0], 6, &mac[0], 6, &ret, nil); err == nil {
		t.mac = net.HardwareAddr(mac)
	}

	// one auto-reset event per direction
	rEvt, _ := windows.CreateEvent(nil, 0, 0, nil)
	wEvt, _ := windows.CreateEvent(nil, 0, 0, nil)
	t.rOvl = &windows.Overlapped{HEvent: rEvt}
	t.wOvl = &windows.Overlapped{HEvent: wEvt}
	return t, nil
}

func (t *tapDatapath) Read() ([]byte, error) {
	buf := make([]byte, 2048)
	var done uint32
	err := windows.ReadFile(t.handle, buf, &done, t.rOvl)
	if err == windows.ERROR_IO_PENDING {
		if _, e := windows.WaitForSingleObject(t.rOvl.HEvent, windows.INFINITE); e != nil {
			return nil, e
		}
		if e := windows.GetOverlappedResult(t.handle, t.rOvl, &done, false); e != nil {
			return nil, e
		}
	} else if err != nil {
		return nil, err
	}
	return buf[:done], nil
}

func (t *tapDatapath) Write(frame []byte) error {
	t.wmu.Lock()
	defer t.wmu.Unlock()
	var done uint32
	err := windows.WriteFile(t.handle, frame, &done, t.wOvl)
	if err == windows.ERROR_IO_PENDING {
		if _, e := windows.WaitForSingleObject(t.wOvl.HEvent, windows.INFINITE); e != nil {
			return e
		}
		if e := windows.GetOverlappedResult(t.handle, t.wOvl, &done, false); e != nil {
			return e
		}
	} else if err != nil {
		return err
	}
	return nil
}

func (t *tapDatapath) MAC() net.HardwareAddr {
	if t.mac != nil {
		return t.mac
	}
	return macFromID("tap")
}

func (t *tapDatapath) Close() error {
	if t.closed {
		return nil
	}
	t.closed = true
	// flip media down, then close
	status := []byte{0, 0, 0, 0}
	var ret uint32
	windows.DeviceIoControl(t.handle, tapSetMediaStatus, &status[0], 4, &status[0], 4, &ret, nil)
	return windows.CloseHandle(t.handle)
}

// configureTAP sets a static IP (no gateway) and a conservative MTU on the adapter.
func configureTAP(name, ip, subnetCIDR string) error {
	mask := maskFromCIDR(subnetCIDR)
	cmds := [][]string{
		{"interface", "ip", "set", "address", "name=" + name, "static", ip, mask},
		{"interface", "ipv4", "set", "subinterface", name, "mtu=1400", "store=persistent"},
	}
	for _, args := range cmds {
		if out, err := exec.Command("netsh", args...).CombinedOutput(); err != nil {
			return fmt.Errorf("netsh %s: %v (%s)", strings.Join(args, " "), err, strings.TrimSpace(string(out)))
		}
	}
	allowLANFirewall(subnetCIDR)
	return nil
}

// allowLANFirewall lets peers on the virtual LAN reach this machine. The adapter defaults to a
// "Public" firewall profile, which blocks inbound ICMP and game/LAN discovery — so we add an
// explicit allow rule scoped to the overlay subnet (and try to mark the network Private). This
// is what makes ping and LAN games actually work; the tunnel itself is already up.
func allowLANFirewall(subnetCIDR string) {
	// idempotent: drop any prior rule, then add
	exec.Command("netsh", "advfirewall", "firewall", "delete", "rule", "name=OrbitLan").Run()
	exec.Command("netsh", "advfirewall", "firewall", "add", "rule", "name=OrbitLan",
		"dir=in", "action=allow", "remoteip="+subnetCIDR).Run()
	// best-effort: put the OrbitLan adapter on the Private profile so discovery is allowed
	exec.Command("powershell", "-NoProfile", "-Command",
		"Set-NetConnectionProfile -InterfaceAlias OrbitLan -NetworkCategory Private -ErrorAction SilentlyContinue").Run()
}

func maskFromCIDR(cidr string) string {
	_, ipnet, err := net.ParseCIDR(cidr)
	if err != nil || len(ipnet.Mask) != 4 {
		return "255.255.255.0"
	}
	return fmt.Sprintf("%d.%d.%d.%d", ipnet.Mask[0], ipnet.Mask[1], ipnet.Mask[2], ipnet.Mask[3])
}
