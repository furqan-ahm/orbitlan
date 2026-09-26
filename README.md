<h1 align="center">
  <img src="website/assets/earth-clouds.gif" width="40" align="texttop" alt="O">rbitLan
</h1>

<p align="center">
  <strong>A private Virtual LAN for everyone.</strong><br>
  Simple enough for game night, useful for anything that works over a local network.<br>
  <sub>Community Edition v1.0.0 is available now.</sub>
</p>

<p align="center">
  <a href="https://github.com/furqan-ahm/orbitlan/releases/latest/download/OrbitLan-Installer.exe">Download for Windows</a>
  ·
  <a href="https://orbitlan.site">Website</a>
  ·
  <a href="https://orbitlan.site/#support">Supporter Edition</a>
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

Gaming was the reason I built it, but it is not limited to games. Direct-IP apps,
development servers, private tools, and other software that works over an IP-based LAN
can technically use OrbitLan too. Application discovery and firewall behavior can vary.

It is the Hamachi/ZeroTier idea, rebuilt to be simple, lightweight, and free.

## Features

- **Direct peer-to-peer.** Network traffic flows straight between devices (~10 ms on good
  paths). It does not route through a middle server, avoiding extra relay latency and server
  bandwidth use.
- **Encrypted.** Every peer-to-peer link is encrypted (ChaCha20-Poly1305).
- **Just a code.** One person creates a network and shares a join code. That's the whole
  setup — no accounts, no logins.
- **Works through tough NATs.** Direct connections are attempted first; when two peers
  are both behind strict NATs, an optional encrypted relay keeps them connected.
- **Free.** Direct connections are free forever. The relay runs from a shared free pool,
  and you can turn the relay off entirely for a guaranteed-free, direct-only experience.

## How it works

```
 Your device ────── direct P2P (encrypted) ────── Other device
       \                                              /
        \── coordinator (who is here + how to connect) ─┘
            never sees application traffic
```

1. The **coordinator** (a Cloudflare Worker) tells peers who's in the network and helps
   them find each other. It's used only to *connect*.
2. Peers then talk **directly**, encrypted, over a virtual network adapter. Applications
   send LAN traffic to `10.69.0.x` and it reaches the right device.
3. If a direct link truly can't form, an optional **relay** carries the encrypted traffic
   as a fallback.

The coordinator helps establish connections and never sees application traffic. If a relay
is required, it forwards encrypted packets but cannot read them.

## Download & install (Windows)

Download the latest [OrbitLan Community installer](https://github.com/furqan-ahm/orbitlan/releases/latest/download/OrbitLan-Installer.exe)
and run it once. Setup installs the virtual network adapter and OrbitLan's small privileged
network service with one Windows approval. After that, open OrbitLan from the Start menu
normally — the UI and notification-area app never request elevation. A
[portable package](https://github.com/furqan-ahm/orbitlan/releases/latest/download/OrbitLan-Portable.zip)
is available too.

## Community and Supporter editions

OrbitLan's networking, encryption, self-hosting, tray mode, and security updates are the
same in both editions. Community is the only edition published on GitHub Releases.

| | Community | Supporter |
|---|---|---|
| Official build | Free v1.0.0 on GitHub | Pay $3 or more through Lemon Squeezy |
| Hosting a network | Up to 4 nodes | Larger rooms, up to the coordinator's safety limit |
| Joining a network | Can join a larger Supporter-hosted room | Can join any room with space |
| Relay choices | Auto or Off | Auto, Off, or Force Relay |
| Visuals | Standard OrbitLan theme | Supporter theme, moon orbit, and welcome animation |

The Supporter Edition is for people who want the ready-to-run extras and want to help fund
development and the shared relay. There is no account, subscription, or DRM inside the app.
OrbitLan remains MIT licensed: the complete source for both flavors is here, and you may
build either one yourself.

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

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-native-release.ps1 -Edition Supporter
```

The script tests and builds the Go network engine, native C++ client, Windows service,
setup helper, and installer. Community packages are written to:

- `dist\OrbitLan-Installer.exe`
- `dist\OrbitLan-Portable.zip`

Supporter packages are kept separate:

- `dist\OrbitLan-Supporter-Installer.exe`
- `dist\OrbitLan-Supporter-Portable.zip`

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

- **Direct connections** never touch any server — traffic goes friend-to-friend, encrypted.
- **Relayed connections** (only when a direct link can't form) pass encrypted traffic
  through Cloudflare's TURN relay. The relay cannot read it.
- You control this with the **relay toggle**: `Off` (direct-only) or `Auto`
  (direct-first, relay fallback — default). Supporter Edition also exposes `On` to force
  relay use when troubleshooting a difficult path.

## Self-hosting

You can run the entire stack yourself for free — no dependency on anyone else's servers:

- **Coordinator**: deploy [`coordinator/`](coordinator/) to your own Cloudflare account
  (free tier). See its [README](coordinator/README.md).
- **Relay**: configure that coordinator with your own Cloudflare TURN key. Direct-only mode
  needs no relay account at all.
- **Client**: set your coordinator endpoint in Settings.

## Support

OrbitLan Community Edition is free, and direct connections do not create relay bandwidth
costs. I build and maintain it in my free time, while the optional shared relay has a real
running cost.

If OrbitLan helps bring an old game night back to life, the Supporter Edition will be sold
through Lemon Squeezy for **$3 or more — you choose the amount**. It adds larger hosted rooms,
Force Relay, and a distinct visual theme without withholding security, self-hosting, or the
core network from Community users. Its checkout link will be added after the store review is
complete; Community v1.0.0 remains immediately available from GitHub.

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
| [`client/`](client/) | Windows client: virtual adapter engine (Go) + native UI/service (C++/Win32) |
| [`website/`](website/) | Marketing/landing site (Cloudflare Pages) |
| `docs/` | Architecture and operational notes |

## License

[MIT](LICENSE). Use it, fork it, self-host it. Bundled driver components retain their own
licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).

Created and maintained by [Furqan Ahmed](https://github.com/furqan-ahm).
