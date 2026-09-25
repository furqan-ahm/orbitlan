# OrbitLan native Windows client

This branch keeps the existing WPF application as the first deliverable and develops a lighter
native Windows client separately. The networking core remains the tested Go engine; the desktop
shell, tray, privilege boundary, and installer are native C++20/Win32.

## Processes

- `OrbitLan.exe` runs as the signed-in user. It owns the window and notification-area icon.
- `OrbitLanService.exe` runs as LocalSystem and owns privileged adapter/engine operations.
- `orbitlan-engine.exe` runs only while connected, as a child of the service in a kill-on-close job.
- `OrbitLanSetup.exe` installs the service, files, shortcut, and uninstall registration.

Setup is the only routine operation that requests elevation. The UI never asks for administrator
rights, and the service performs TAP installation, adapter configuration, firewall changes, and
priority changes after installation.

## Security boundary

The UI talks to the service through `\\.\pipe\OrbitLan.Control.v1`. Its ACL permits LocalSystem,
administrators, and interactive users. Fields that may contain spaces or protocol delimiters are
base64 encoded, length-limited, and validated again by the service.

The engine control API remains loopback-only and now requires a random 256-bit token generated for
each engine launch. Only the service receives that token. Shutdown is graceful, with a bounded job
termination fallback.

## Visual performance

The UI uses GDI/GDI+ rather than WPF. It reuses the approved Earth/cloud GIF and draws the gradient,
stars, connection pulse, and node orbit natively. Visual timers run only while the window is visible.
Hiding the window destroys the GIF decoder, stops the animation timer, and trims the process working
set. Network status polling and the tray remain active.

## Build

Requirements:

- Visual Studio with the Desktop development with C++ workload
- CMake 3.24 or newer
- Windows 10 SDK
- Go version declared in `client/engine/go.mod`

From a Developer PowerShell:

```powershell
cmake -S client/native -B build/native -A x64
cmake --build build/native --config Release --parallel
```

Build the complete release package with:

```powershell
.\scripts\build-native-release.ps1
```

The output is `dist\OrbitLan-Native.zip`. Run `OrbitLanSetup.exe` from the extracted directory once,
then launch OrbitLan from the Start menu without elevation.

Pass `-SigningThumbprint` to sign the four OrbitLan executables with an installed code-signing
certificate and RFC 3161 timestamp. Unsigned packages are suitable for development only.

## Measured development build

Measured on the same 12-logical-processor Windows machine used for the WPF baseline:

| State | Working set | CPU |
| --- | ---: | ---: |
| Native UI visible with Earth/stars | 22–23 MB | about 0.5% total CPU |
| Native UI hidden in tray | 2.9 MB | below the 10-second measurement resolution |
| Privileged service, idle | 7.7–9 MB | below the 10-second measurement resolution |
| Expected connected tray total, including measured Go engine | about 34 MB | traffic-dependent |

The current WPF tray build measured about 166 MB including the engine. These figures are development
measurements, not minimum system requirements; release signing and physical multi-PC load tests remain
release gates.
