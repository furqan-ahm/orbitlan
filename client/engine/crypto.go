package main

import (
	"crypto/cipher"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"io"
	"sync"

	"golang.org/x/crypto/chacha20poly1305"
	"golang.org/x/crypto/hkdf"
)

// Per-pair, per-direction AEAD. The network's join code is the shared secret; from it we derive
// a distinct key for each ordered pair (me->peer), so a monotonic counter can serve as the nonce
// with no risk of (key,nonce) reuse across different peers. Each frame on the wire is:
//
//	[ counter(8, big-endian) | ciphertext(frame) + tag(16) ]
//
// The counter is also the AEAD nonce (zero-padded to 12 bytes) and drives replay protection.
//
// NOTE (v1 honesty): the key is derived purely from the join code, so anyone with the code can
// derive all keys — there is no per-peer forward secrecy. A Noise/X25519 handshake per pair is
// the planned hardening; this is a shared-network-key model suitable for a small trusted group.

type sealer struct {
	send    cipher.AEAD
	recv    cipher.AEAD
	sendCtr uint64
	mu      sync.Mutex
	// replay window for the recv direction
	rmu     sync.Mutex
	highest uint64
	window  uint64 // bitmap of the 64 counters below `highest`
}

func deriveKey(joinCode, info string) []byte {
	r := hkdf.New(sha256.New, []byte(joinCode), []byte("orbitlan-v1-psk"), []byte(info))
	key := make([]byte, chacha20poly1305.KeySize)
	io.ReadFull(r, key)
	return key
}

// newSealer builds directional AEADs for the ordered pair (myID, peerID).
func newSealer(joinCode, myID, peerID string) (*sealer, error) {
	sk := deriveKey(joinCode, myID+"->"+peerID)
	rk := deriveKey(joinCode, peerID+"->"+myID)
	send, err := chacha20poly1305.New(sk)
	if err != nil {
		return nil, err
	}
	recv, err := chacha20poly1305.New(rk)
	if err != nil {
		return nil, err
	}
	return &sealer{send: send, recv: recv}, nil
}

func (s *sealer) seal(frame []byte) []byte {
	s.mu.Lock()
	ctr := s.sendCtr
	s.sendCtr++
	s.mu.Unlock()

	out := make([]byte, 8, 8+len(frame)+chacha20poly1305.Overhead)
	binary.BigEndian.PutUint64(out, ctr)
	var nonce [12]byte
	binary.BigEndian.PutUint64(nonce[4:], ctr)
	return s.send.Seal(out, nonce[:], frame, out[:8]) // counter is AAD too
}

func (s *sealer) open(pkt []byte) ([]byte, error) {
	if len(pkt) < 8+chacha20poly1305.Overhead {
		return nil, fmt.Errorf("short packet")
	}
	ctr := binary.BigEndian.Uint64(pkt[:8])
	var nonce [12]byte
	binary.BigEndian.PutUint64(nonce[4:], ctr)
	frame, err := s.recv.Open(nil, nonce[:], pkt[8:], pkt[:8])
	if err != nil {
		return nil, err
	}
	if !s.acceptCounter(ctr) {
		return nil, fmt.Errorf("replay/duplicate counter %d", ctr)
	}
	return frame, nil
}

// acceptCounter implements an anti-replay sliding window (RFC 6479 style, 64-wide).
func (s *sealer) acceptCounter(ctr uint64) bool {
	s.rmu.Lock()
	defer s.rmu.Unlock()
	const W = 64
	if ctr > s.highest {
		shift := ctr - s.highest
		if shift >= W {
			s.window = 0
		} else {
			s.window <<= shift
		}
		s.window |= 1
		s.highest = ctr
		return true
	}
	diff := s.highest - ctr
	if diff >= W {
		return false // too old
	}
	mask := uint64(1) << diff
	if s.window&mask != 0 {
		return false // already seen
	}
	s.window |= mask
	return true
}
