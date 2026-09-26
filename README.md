<p align="center">
  <img src="website/assets/earth-clouds.gif" width="220" alt="OrbitLan pixel-art Earth spinning beneath moving clouds">
</p>

<h1 align="center">OrbitLan</h1>

<p align="center">
  <strong>LAN games. Any distance. One short code.</strong><br>
  A small, free peer-to-peer virtual LAN built for bringing old game nights back.
</p>

<p align="center">
  <a href="https://github.com/furqan-ahm/orbitlan/releases/latest/download/OrbitLan-Installer.exe">Download for Windows</a>
  ·
  <a href="https://orbitlan-web.furqan-ahm.workers.dev">Website</a>
  ·
  <a href="https://patreon.com/orbitlan">Support the project</a>
</p>

---

## Why I made this

I hadn't worked on anything just for fun in a while, so I started building OrbitLan in
my free time. I wanted an easy way to play the old games I grew up with—especially
**Need for Speed: Most Wanted** and **Battlefield 1942**—with friends again.

I didn't want everyone to create accounts or spend half the evening wrestling with VPN
settings. With OrbitLan, one person creates a room, shares a short code, and the game
sees everyone as if they were connected to the same router.

That's it. A small project for old games, good friends, and one more match.

## What it is

OrbitLan puts you and your friends on one virtual local network over the internet. Old
and new games that only support **LAN / direct-IP** multiplayer — the ones with no online
servers, or servers long gone — just work again, because to the game you're all on the
same subnet (`10.69.0.x`).

It's the Hamachi/ZeroTier idea, rebuilt to be simple, fast, and free.

## Features

- **Direct peer-to-peer.** Game traffic flows straight between players (~10 ms on good
  paths). It never routes through a middle server, so there's no added lag and no bandwidth
  cost to anyone.
- **Encrypted.** Every peer-to-peer link is encrypted (ChaCha20-Poly1305).
- **Just a code.** One person creates a network and shares a join code. That's the whole
  setup — no accounts, no logins.
- **Works through tough NATs.** Direct connections are attempted first; when two players
  are both behind strict NATs, an optional encrypted relay keeps them connected.
- **Free.** Direct play is free forever. The relay runs from a shared free pool, and you
  can turn the relay off entirely for a guaranteed-free, direct-only experience.

## How it works

```
   You ──────────── direct P2P (encrypted) ──────────── Friend
     \                                                   /
      \── coordinator (setup only: who's here + how to  ─┘
          reach each other) — never sees game traffic
```

1. The **coordinator** (a Cloudflare Worker) tells peers who's in the network and helps
   them find each other. It's used only to *connect*.
2. Peers then talk **directly**, encrypted, over a virtual network adapter. Your games
   send LAN traffic to `10.69.0.x` and it reaches the right friend.
3. If a direct link truly can't form, an optional **relay** carries the encrypted traffic
   as a fallback.

The coordinator and relay are used only to establish the connection — the gameplay itself
is peer-to-peer.

## Download & install (Windows)

Download the latest [OrbitLan installer](https://github.com/furqan-ahm/orbitlan/releases/latest/download/OrbitLan-Installer.exe)
and run it once. Setup installs the virtual network adapter and OrbitLan's small privileged
network service with one Windows approval. After that, open OrbitLan from the Start menu
normally — the UI and notification-area app never request elevation. A
[portable package](https://github.com/furqan-ahm/orbitlan/releases/latest/download/OrbitLan-Portable.zip)
is available too.

## Relay & privacy

- **Direct connections** never touch any server — traffic goes friend-to-friend, encrypted.
- **Relayed connections** (only when a direct link can't form) pass encrypted traffic
  through Cloudflare's TURN relay. The relay cannot read it.
- You control this with the **relay toggle**: `Off` (direct-only, fully free),
  `Auto` (direct-first, relay fallback — default), or `On`.

## Self-hosting

You can run the entire stack yourself for free — no dependency on anyone else's servers:

- **Coordinator**: deploy [`coordinator/`](coordinator/) to your own Cloudflare account
  (free tier). See its [README](coordinator/README.md).
- **Relay**: configure that coordinator with your own Cloudflare TURN key. Direct-only mode
  needs no relay account at all.
- **Client**: set your coordinator endpoint in Settings.

## Support

OrbitLan is free and always will be for direct play. I build and maintain it in my free
time, while the optional shared relay has a real running cost.

If you guys like OrbitLan and it helps bring an old game night back to life, please
consider **[supporting me on Patreon](https://patreon.com/orbitlan)**. It is completely
optional, but it would genuinely mean a lot and help me keep working on it.

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
