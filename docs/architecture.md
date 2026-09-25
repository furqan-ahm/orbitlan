# Architecture

OrbitLan has a small control plane and a peer-to-peer data plane.

## Control plane

The Cloudflare Worker in `coordinator/` exposes four room operations: join, signal, poll,
and leave. Each room code maps to one Durable Object, which owns that room's short-lived
membership list, virtual IP allocation, and ICE signaling mailbox.

The coordinator never receives Ethernet frames or game traffic. When relay support is enabled,
it mints short-lived Cloudflare TURN credentials and returns them with the join response.

## Data plane

The Go engine opens the local TAP-Windows adapter and creates one Pion ICE connection to every
other peer in the room. ICE tries direct UDP first in `auto` mode. The user can disable TURN
entirely with `off`, or force relay candidates for troubleshooting with `on`.

Ethernet frames are encrypted per peer with ChaCha20-Poly1305. Keys are derived from the room
code and the two peer identifiers. A MAC-learning switch forwards known unicast traffic to one
peer, replicates broadcast and multicast traffic, and answers known peer ARP requests locally.

## Local application boundary

The WPF app launches the engine as a child process and talks to its loopback-only HTTP status
API at `127.0.0.1:9099`. The UI is responsible for first-run TAP installation, user settings,
connection status, and cryptographically signed application updates.

## Trust boundaries

- The room code is a shared secret. Anyone who knows it can join that room.
- Cloudflare can observe coordinator and relay metadata but cannot decrypt game frames.
- Direct peers necessarily learn one another's public network addresses during ICE.
- Update packages are accepted only when both their RSA signature and SHA-256 hash validate.
