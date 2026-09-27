# OrbitLan for Linux (headless)

The Linux client is the same Go networking engine used by the Windows app, with a native
Linux TAP datapath. It carries layer-2 Ethernet frames, so it can serve LAN games as well as
servers and private administration networks.

## Run in the terminal

Preview the terminal dashboard without root access or a network connection:

```bash
./orbitlan demo
```

The client needs permission to create and configure `/dev/net/tun`. Creating or joining a
room launches a small background engine, so the connection does not depend on an open terminal:

```bash
# Create a room, print its six-character code, and connect in the background
sudo ./orbitlan new --name alice

# Join an existing room in the background
sudo ./orbitlan join ABC123 --name bob
```

Once connected, control it from an ordinary terminal without `sudo`:

```bash
./orbitlan status              # one-time summary
./orbitlan nodes               # names, IPs, paths, latency, and node IDs
./orbitlan watch               # attach the live dashboard
./orbitlan kick 10.69.0.4      # host only; name or node ID also works
./orbitlan logs                # last 40 background-engine messages
./orbitlan leave               # disconnect and remove orbitlan0
```

`Ctrl+C` in `watch` closes only the status view. The connection keeps running in the
background. Use `leave` when you actually want to disconnect. Only the host can kick, and
the coordinator verifies that permission rather than trusting the terminal interface.

For troubleshooting, `sudo ./orbitlan run ABC123 --name alice` keeps the engine in the
foreground. Its dashboard owns that foreground session, so `Ctrl+C` disconnects it.

The public coordinator and automatic direct-first relay mode are defaults. Self-hosters can
override them:

```bash
sudo ./orbitlan join ABC123 \
  --name game-server \
  --server https://your-coordinator.example \
  --relay off
```

OrbitLan creates an ephemeral `orbitlan0` TAP interface, assigns its `10.69.0.x/24` room
address, sets MTU 1400, and removes the interface on `leave`. Install `iproute2` if the `ip`
command is unavailable.

The live dashboard is an attachable view, not the connection process. It redraws once per
second while `watch` is open and consumes no resources when it is closed.

### WSL

WSL2 is supported when `/dev/net/tun` is available. The demo works without elevated access;
real rooms use the same `sudo ./orbitlan ...` commands shown above. WSL1 does not provide the
network features required by the TAP datapath.

Linux applications inside WSL2 can use the OrbitLan address normally. Windows games should
still use the Windows OrbitLan app: Windows does not automatically treat a TAP interface inside
its WSL2 VM as a Windows LAN adapter, and forwarding game discovery across that boundary needs
extra routing and firewall configuration.

Linux firewalls are distribution-specific. If peers connect but cannot reach a hosted game
or service, allow the required application ports on `orbitlan0` or from `10.69.0.0/24`.

## Run continuously with systemd

The release archive includes an installer, a service unit, and a configuration template:

```bash
sudo ./install.sh
sudo editor /etc/orbitlan/orbitlan.conf
sudo systemctl enable --now orbitlan
sudo systemctl status orbitlan
journalctl -u orbitlan -f
```

Use `sudo orbitlan status`, `sudo orbitlan watch`, and the other control commands for a
systemd-owned connection; its control socket is root-owned. Use `sudo systemctl stop orbitlan`
to disconnect. The service stores its stable peer identity in `/var/lib/orbitlan`.

## Build

Go 1.26 or newer is required:

```bash
cd client/engine
go test ./...
CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o orbitlan .
```

From the repository root, `./scripts/build-linux-release.sh` creates static `amd64` and
`arm64` Community archives under `dist/linux/community/`. For Supporter packages, run
`EDITION=supporter ./scripts/build-linux-release.sh`; those archives are written under
`dist/linux/supporter/` and default to the Supporter edition.

## Private infrastructure use

For an Oracle fleet, use the systemd form and a dedicated room code. Treat OrbitLan as a
management network, not the only recovery path: retain OCI console access and ordinary SSH
so a coordinator or client problem cannot lock you out of a VM.
