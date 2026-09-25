# OrbitLan Windows client

The client has two parts:

- `engine/`: Go networking engine. It owns the TAP adapter, coordinator connection,
  encrypted P2P mesh, ICE negotiation, and relay fallback.
- `ui/`: .NET WPF desktop app. It installs the signed TAP driver, launches the engine,
  shows peers and connection status, and applies signed updates.

## Requirements

- Windows 10 or later, x64
- Go 1.25 or later
- .NET 10 SDK
- PowerShell 5.1 or later

## Build

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
```

The script tests and builds the Go engine, publishes a self-contained Windows app,
and creates `dist/OrbitLan.zip`.

## Configuration

User configuration is stored at `%LOCALAPPDATA%\OrbitLan\config.json`:

```json
{
  "coordinatorURL": "https://orbitlan.furqan-ahm.workers.dev",
  "relayMode": "auto",
  "priority": false
}
```

The coordinator URL and relay mode can also be changed in Settings. Relay modes are:

- `off`: host and server-reflexive ICE candidates only; never uses TURN.
- `auto`: direct-first with TURN fallback.
- `on`: relay candidates only, intended for troubleshooting.

The official coordinator never receives game traffic. It is used only for room membership,
signaling, and short-lived TURN credentials.
