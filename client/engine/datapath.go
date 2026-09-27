package main

import (
	"crypto/sha256"
	"log"
	"net"
	"sync/atomic"
)

// Datapath is the local end of the virtual LAN: the OS-facing side that produces Ethernet
// frames to send to peers (Read) and accepts frames from peers to hand to the OS (Write).
// The real implementation is the TAP adapter; the loopback implementation is for testing the
// mesh + crypto + switching without a driver.
type Datapath interface {
	Read() ([]byte, error) // one Ethernet frame from the OS
	Write([]byte) error    // one Ethernet frame to the OS
	MAC() net.HardwareAddr
	Close() error
}

// macFromID makes a stable, locally-administered unicast MAC from a peer id, so the loopback
// datapath and tests have a deterministic address. (Real TAP uses the adapter's own MAC.)
func macFromID(id string) net.HardwareAddr {
	h := sha256.Sum256([]byte("orbitlan-mac:" + id))
	mac := make(net.HardwareAddr, 6)
	copy(mac, h[:6])
	mac[0] = (mac[0] | 0x02) & 0xFE // locally administered, unicast
	return mac
}

// loopbackDatapath: Read() delivers injected frames; Write() captures/counts them. Used by the
// -selftest harness to prove frames traverse the mesh end to end.
type loopbackDatapath struct {
	mac     net.HardwareAddr
	inject  chan []byte
	written atomic.Int64
	onWrite func([]byte)
	closed  chan struct{}
}

func newLoopback(id string, onWrite func([]byte)) *loopbackDatapath {
	return &loopbackDatapath{
		mac:     macFromID(id),
		inject:  make(chan []byte, 256),
		onWrite: onWrite,
		closed:  make(chan struct{}),
	}
}

func (l *loopbackDatapath) Read() ([]byte, error) {
	select {
	case f := <-l.inject:
		return f, nil
	case <-l.closed:
		return nil, net.ErrClosed
	}
}

func (l *loopbackDatapath) Write(frame []byte) error {
	l.written.Add(1)
	if l.onWrite != nil {
		l.onWrite(frame)
	}
	return nil
}

func (l *loopbackDatapath) MAC() net.HardwareAddr { return l.mac }

func (l *loopbackDatapath) Close() error {
	select {
	case <-l.closed:
	default:
		close(l.closed)
	}
	return nil
}

// Inject feeds a frame as if the OS produced it on the TAP.
func (l *loopbackDatapath) Inject(frame []byte) {
	select {
	case l.inject <- frame:
	default:
		log.Println("loopback inject queue full, dropping")
	}
}
