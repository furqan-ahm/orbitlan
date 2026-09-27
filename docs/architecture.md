# OrbitLan architecture

OrbitLan has a small control plane and a peer-to-peer data plane.

## Control plane

The Cloudflare Worker in `coordinator/` exposes four room operations: join, signal, poll,
and leave. Each room code maps to one Durable Object, which owns that room's short-lived
membership list, virtual IP allocation, and ICE signaling mailbox.

The coordinator never receives Ethernet frames or game traffic. When relay support is enabled,
it mints short-lived Cloudflare TURN credentials and returns them with the join response.

## Data plane

The shared Go engine opens OrbitLan's TAP adapter on Windows or a TAP interface through
`/dev/net/tun` on Linux. It creates one Pion ICE connection to every other peer in the room.
ICE tries direct UDP first in `auto` mode. The user can disable TURN entirely with `off`, or
Supporter builds can force relay candidates for troubleshooting with `on`.

Ethernet frames are encrypted per peer with ChaCha20-Poly1305. Keys are derived from the room
code and the two peer identifiers. A MAC-learning switch forwards known unicast traffic to one
peer, replicates broadcast and multicast traffic, and answers known peer ARP requests locally.

## Windows application boundary

The native C++/Win32 UI runs as the signed-in user and communicates with the LocalSystem
network service through `\\.\pipe\OrbitLan.Control.v1`. The service owns privileged adapter
and firewall operations, validates requests again, and starts the Go engine only while a room
is connected.

The service gives each engine process a random 256-bit control token. Its HTTP control API
listens only on `127.0.0.1:9099` and rejects requests without that token. The engine runs in a
kill-on-close Windows job so stopping the service also stops its data plane.

## Linux application boundary

The Linux command-line client and data plane are the same Go binary. Creating or joining a
room starts a background process that owns `orbitlan0`; later commands use a local Unix socket
under `/run/orbitlan`. The runtime directory and socket permissions restrict control access.
The optional systemd unit runs the same client continuously with its state under
`/var/lib/orbitlan`.

## Trust boundaries

- The room code is a six-character shared secret. Anyone who knows it can join the room and
  derive its traffic keys, so codes should be temporary and shared only with trusted people.
- The coordinator endpoint does not carry game frames, and the relay process receives
  ciphertext rather than plaintext. Because v1 traffic keys derive from the room code sent to
  the coordinator, this is not a zero-knowledge design and does not protect against an operator
  able to combine coordinator state with captured relay traffic.
- Direct peers necessarily learn one another's public network addresses during ICE.
- Windows adapter and firewall changes are performed by the installed service, not the UI.
- Linux needs network-administrator privileges to create and configure its TAP interface.
