# OrbitLan clients

The client tree has four runtime parts:

- `engine/`: Go networking engine. It owns the TAP adapter, coordinator connection,
  encrypted P2P mesh, ICE negotiation, and relay fallback. It has native Windows and Linux
  TAP datapaths.
- `native/`: C++/Win32 desktop UI, notification-area app, Windows service, and one-time setup.
- `ui/`: legacy .NET/WPF prototype and shared visual assets. The WPF application is retained
  for reference but is not part of current release packages.
- `linux/`: headless terminal usage, systemd service, and installer assets for `amd64` and
  `arm64` Linux systems.

## Requirements

- Windows 10 or later, x64 (Windows 7/8 are not supported by the current Go engine)
- Go version declared in `engine/go.mod`
- Visual Studio with Desktop development with C++
- CMake 3.24 or later
- PowerShell 5.1 or later for release packaging

## Build

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-native-release.ps1
```

The script tests and builds the Go engine and three statically linked native executables,
then writes the installer and portable archive under `dist/Community/`. Pass
`-Edition Supporter` to write the corresponding packages under `dist/Supporter/`.

## Configuration

User configuration is stored at `%LOCALAPPDATA%\OrbitLan\native.ini`. It contains the display
name, coordinator URL, relay mode, and adapter-priority preference. No credentials or room codes
are persisted.

Relay modes are:

- `off`: host and server-reflexive ICE candidates only; never uses TURN.
- `auto`: direct-first with TURN fallback.
- `on`: relay candidates only, intended for troubleshooting.

The native UI keeps the Earth/cloud animation, stars, connection pulse, and node visualization
while visible. Minimizing destroys the GIF decoder, stops visual timers, trims the working set,
and leaves the connection manageable from the notification-area icon.

The official coordinator never receives game traffic. It is used only for room membership,
signaling, and short-lived TURN credentials.

## Linux headless client

The Linux binary includes a lightweight terminal dashboard. Preview it without root access,
creating an adapter, or contacting the coordinator:

```bash
./orbitlan demo
```

Build both static Linux packages from the repository root:

```bash
./scripts/build-linux-release.sh
```

See [`linux/README.md`](linux/README.md) for interactive commands, systemd installation,
firewall guidance, and private infrastructure use.
