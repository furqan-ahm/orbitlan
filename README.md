<h1 align="center">
  <img src="website/assets/earth-clouds.gif" width="40" align="texttop" alt="O">rbitLan
</h1>

<p align="center">
  <strong>A private Virtual LAN for everyone.</strong><br>
  Simple enough for game night, useful for anything that works over a local network.<br>
  <sub>Community Edition v1.0.1 is available now.</sub>
</p>

<p align="center">
  <a href="https://github.com/furqan-ahm/orbitlan/releases/latest/download/OrbitLan-Installer.exe">Download for Windows</a>
  ·
  <a href="https://orbitlan.site">Website</a>
  ·
  <a href="https://www.patreon.com/orbitlan/posts/orbitlan-edition-170621986">Supporter Edition — $5 once</a>
</p>

---

## Why I made this

I hadn't built anything just for fun in a while. I made OrbitLan in my free time so I
could play old games like **Need for Speed: Most Wanted** and **Battlefield 1942** with
friends again—without accounts or a complicated VPN setup.

Create a room, share a short code, and your devices behave as if they were connected to
the same router.

## What it is

OrbitLan connects trusted devices through an encrypted virtual local network over the
internet. Each device gets an address on the same private subnet (`10.69.0.x`) and can
communicate directly with the others.

Gaming was the reason I built it, but it is not limited to games. Minecraft Java servers
and Direct Connection, other compatible LAN games, development servers, private tools,
and direct-IP apps can use OrbitLan too. Application discovery and firewall behavior vary.

It follows the familiar Hamachi/ZeroTier idea, with a smaller room-code workflow and an
open-source, self-hostable stack.

## Features

- **Direct peer-to-peer.** Application traffic flows straight between devices when ICE can
  establish a direct path. Latency depends on the peers' real networks and physical distance.
- **Encrypted.** Every peer-to-peer link is encrypted (ChaCha20-Poly1305).
- **Just a code.** One person creates a network and shares a join code. That's the whole
  setup — no accounts, no logins.
- **Works through tough NATs.** Direct connections are attempted first; when two peers
  are both behind strict NATs, an optional encrypted relay keeps them connected.
- **Free Community build.** Direct paths do not consume shared relay bandwidth. Relay can be
  disabled for direct-only operation, and the complete stack can be self-hosted.

## How it works

```
 Your device ────── direct P2P (encrypted) ────── Other device
       \                                              /
        \── coordinator (who is here + how to connect) ─┘
            does not carry application traffic
```

1. The **coordinator** (a Cloudflare Worker) tells peers who's in the network and helps
   them find each other. It's used only to *connect*.
2. Peers then talk **directly**, encrypted, over a virtual network adapter. Applications
   send LAN traffic to `10.69.0.x` and it reaches the right device.
3. If a direct link truly can't form, an optional **relay** carries the encrypted traffic
   as a fallback.

The coordinator endpoint helps establish connections and does not carry application traffic.
If a relay is required, it forwards encrypted packets. In the current v1 design, however,
traffic keys are derived from the room code sent to the coordinator. Encryption protects the
traffic in transit, but it must not be treated as protection from a service operator able to
combine coordinator data with captured relay traffic.

Room codes are intentionally short for game-night convenience. Treat them as temporary
invitations, not as long-term high-entropy passwords: share them privately and create a new
room if a code is exposed.

## Download & install

### Windows

Download the latest [OrbitLan Community installer](https://github.com/furqan-ahm/orbitlan/releases/latest/download/OrbitLan-Installer.exe)
and run it once. Setup installs the virtual network adapter and OrbitLan's small privileged
network service with one Windows approval. After that, open OrbitLan from the Start menu
normally — the UI and notification-area app never request elevation. A
[portable package](https://github.com/furqan-ahm/orbitlan/releases/latest/download/OrbitLan-Portable.zip)
is available too.

### Linux (headless preview)

> **Preview:** ready-to-run Community packages are published for `amd64` and `arm64`, but
> commands and packaging may still change while Linux support is completed.

Download the package for your system from the
[latest GitHub release](https://github.com/furqan-ahm/orbitlan/releases/latest) or the
[OrbitLan download page](https://orbitlan.site/download/#linux). Extract it and run
`sudo ./install.sh`; the archive includes the binary, systemd unit, configuration example,
and detailed README.

The native Linux TAP client can create or join the same OrbitLan rooms from a terminal. It
supports interactive use and a hardened systemd service on `amd64` and `arm64`:

```bash
# Preview the dashboard without connecting
orbitlan demo

sudo orbitlan new --name alice
sudo orbitlan join ABC123 --name bob

# The LAN stays connected in the background
orbitlan status
orbitlan watch
orbitlan nodes
orbitlan leave
```

It is testable in WSL2 when `/dev/net/tun` is available. Linux software inside WSL can use
the virtual LAN directly; use the Windows client for Windows games because Windows does not
automatically expose WSL's internal TAP interface as a Windows LAN adapter.

To build the Linux packages yourself, use
[`scripts/build-linux-release.sh`](scripts/build-linux-release.sh). Complete installation,
firewall, service, and cloud-VM instructions are in the
[`client/linux` guide](client/linux/README.md).

New to virtual LANs? These short guides cover the common setups:

- [Play compatible LAN games online with friends](https://orbitlan.site/guides/play-lan-games-online/)
- [Play Minecraft Java without router port forwarding](https://orbitlan.site/guides/minecraft-java-without-port-forwarding/)
- [Use OrbitLan as a simple open-source Hamachi alternative](https://orbitlan.site/guides/hamachi-alternative/)
- [Build and self-host OrbitLan with Cloudflare or an Oracle relay](https://orbitlan.site/guides/self-host-orbitlan/)

## Community and Supporter editions

OrbitLan's networking, encryption, self-hosting, tray mode, and security updates are the
same in both editions. Community builds for Windows and Linux are published on GitHub Releases.

| | Community | Supporter |
|---|---|---|
| Official build | Free v1.0.1 on GitHub | One-time $5 purchase through Patreon |
| Hosting on OrbitLan's public service | 5 computers total: host + 4 others | Larger rooms, up to the coordinator's safety limit |
| Joining a network | Can join a larger Supporter-hosted room | Can join any room with space |
| Relay choices | Auto or Off | Auto, Off, or Force Relay |
| Visuals | Standard OrbitLan theme | Supporter theme, moon orbit, and welcome animation |

The Supporter Edition is for people who want the ready-to-run extras and want to help fund
development and the shared relay. There is no account, subscription, or DRM inside the app.
OrbitLan remains MIT licensed: the complete source for both flavors is here, and you may
build either one yourself.

The five-computer Community limit applies only when hosting through OrbitLan's public
coordinator. A self-hosted coordinator has no Community/Supporter edition restriction:
set `COMMUNITY_MEMBER_CAP = "0"` and choose your own `MAX_MEMBERS` safety cap (up to the
current `/24` subnet ceiling of 241 members).

## Uninstall

For the installed version:

1. Disconnect from OrbitLan and close the app.
2. Open **Windows Settings → Apps → Installed apps** (or **Apps & features** on
   Windows 10).
3. Find **OrbitLan**, select **Uninstall**, and approve the Windows administrator prompt.

For the portable version, uninstall **OrbitLan Network Components** from the same Windows
screen, then delete the folder where you extracted `OrbitLan-Portable.zip`.

The uninstaller removes OrbitLan's background service, owned TAP adapter, firewall rule,
Start Menu shortcut, and installed program files. If Windows reports that anything is still
in use, restart the computer to finish removal.

OrbitLan keeps personal settings separately. For a completely clean removal, you can also
delete `%LOCALAPPDATA%\OrbitLan` (settings) and `%PROGRAMDATA%\OrbitLan` (setup log) after
uninstalling.

## Build from source

The native Windows client requires:

- Windows 10 or 11, x64
- Git
- Go (the version declared in [`client/engine/go.mod`](client/engine/go.mod))
- Visual Studio with the **Desktop development with C++** workload and Windows SDK
- CMake 3.24 or newer
- PowerShell 5.1 or newer

From a Developer PowerShell, run:

```powershell
git clone https://github.com/furqan-ahm/orbitlan.git
cd orbitlan
powershell -ExecutionPolicy Bypass -File .\scripts\build-native-release.ps1
```

That builds Community Edition. To build Supporter Edition from the same open source:

```bash
EDITION=supporter VERSION=1.0.1 ./scripts/build-linux-release.sh
```

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-native-release.ps1 -Edition Supporter
```

The script tests and builds the Go network engine, native C++ client, Windows service,
setup helper, and installer. Community packages are written to:

- `dist\Community\OrbitLan-Installer.exe`
- `dist\Community\OrbitLan-Portable.zip`

Supporter packages are kept separate:

- `dist\Supporter\OrbitLan-Supporter-Installer.exe`
- `dist\Supporter\OrbitLan-Supporter-Portable.zip`
- `dist\Supporter\OrbitLan-Supporter-v1.0.1.zip` (Patreon delivery bundle, including Linux)

To work on the Cloudflare coordinator locally, install Node.js 22 or newer and run:

```powershell
cd coordinator
npm ci
npm run typecheck
npm run dev
```

More details are available in the [native client](client/native/README.md) and
[coordinator](coordinator/README.md) documentation.

## Relay & privacy

- **Direct application traffic** goes friend-to-friend and does not pass through a relay;
  coordinator and STUN traffic are still used to establish the path.
- **Relayed connections** pass encrypted traffic through a TURN relay when fallback is needed
  or Force Relay is selected. The relay process is not handed a decryption key, but v1 keys
  are derived from the room code known to the coordinator; do not treat this as an
  operator-proof or zero-knowledge design.
- You control this with the **relay toggle**: `Off` (direct-only) or `Auto`
  (direct-first, relay fallback — default). Supporter Edition also exposes `On` to force
  relay use when troubleshooting a difficult path.

## Self-hosting

You can run the entire stack yourself for free — no dependency on anyone else's servers:

- **Coordinator**: deploy [`coordinator/`](coordinator/) to your own Cloudflare account
  (subject to the current Workers limits). See its [README](coordinator/README.md).
- **Room size**: set `COMMUNITY_MEMBER_CAP = "0"` to remove edition-based room limits,
  then choose the operator-controlled `MAX_MEMBERS` cap that fits your deployment.
- **Relay**: configure that coordinator with your own Cloudflare TURN key, or connect it
  to coturn on an Oracle Cloud VM. Direct-only mode needs no relay account at all.
- **Client**: set your coordinator endpoint in Settings.

The website's [complete self-hosting guide](https://orbitlan.site/guides/self-host-orbitlan/)
covers requirements, client builds, both relay choices, firewall rules, and verification.

## Support

OrbitLan Community Edition is free, and direct connections do not create relay bandwidth
costs. I build and maintain it in my free time, while the optional shared relay has a real
running cost.

If OrbitLan helps bring an old game night back to life, the
[Supporter Edition is available on Patreon](https://www.patreon.com/orbitlan/posts/orbitlan-edition-170621986)
for a **one-time $5 purchase**. It adds larger hosted rooms,
Force Relay, and a distinct visual theme without withholding security, self-hosting, or the
core network from Community users. It does not require a recurring membership; checkout and
private downloads are handled by Patreon. Community v1.0.1 remains immediately available
from GitHub.

## Help and feedback

If you find a bug, have a feature idea, or need help with a reproducible problem, please
[open a GitHub issue](https://github.com/furqan-ahm/orbitlan/issues). That also lets other
people find and benefit from the answer.

For help that should not be discussed publicly, email
[furqanshaheer@gmail.com](mailto:furqanshaheer@gmail.com).

## Repository layout

| Path | What |
|---|---|
| [`coordinator/`](coordinator/) | Cloudflare Worker + Durable Object signaling/TURN coordinator |
| [`client/`](client/) | Shared Go network engine, native Windows UI/service, and Linux packaging |
| [`website/`](website/) | Marketing/landing site (Cloudflare Pages) |
| [`docs/architecture.md`](docs/architecture.md) | Current control plane, data plane, and local security boundaries |

## License

[MIT](LICENSE). Use it, fork it, self-host it. Bundled driver components retain their own
licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).

Created and maintained by [Furqan Ahmed](https://github.com/furqan-ahm).
