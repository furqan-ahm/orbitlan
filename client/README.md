# OrbitLan Windows client

The native branch has three runtime parts:

- `engine/`: Go networking engine. It owns the TAP adapter, coordinator connection,
  encrypted P2P mesh, ICE negotiation, and relay fallback.
- `native/`: C++/Win32 desktop UI, notification-area app, Windows service, and one-time setup.
- `ui/`: the original .NET/WPF deliverable retained for comparison and maintenance on `main`.

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
then creates `dist/OrbitLan-Native.zip`.

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
