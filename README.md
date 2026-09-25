<h1 align="center">OrbitLan</h1>

<p align="center">A free, peer-to-peer virtual LAN for gaming with friends across cities and ISPs.<br>
Create a network, share a code, and your games see everyone as if they were on the same router.</p>

---

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

Grab the latest release from the [Releases](../../releases) page, unzip, and run
`OrbitLan.exe`. On first connect it installs a signed virtual network adapter (a one-time
Windows prompt). The app auto-updates itself (signed updates only).

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

OrbitLan is free and always will be for direct play. If it's useful to you and you'd like
to help cover the shared relay pool, there's a **Support** button in the app — entirely
optional.

## Repository layout

| Path | What |
|---|---|
| [`coordinator/`](coordinator/) | Cloudflare Worker + Durable Object signaling/TURN coordinator |
| [`client/`](client/) | Windows client: virtual adapter engine (Go) + WPF UI (C#) |
| [`website/`](website/) | Marketing/landing site (Cloudflare Pages) |
| `docs/` | Architecture and operational notes |

## License

[MIT](LICENSE). Use it, fork it, self-host it. Bundled driver components retain their own
licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).

Created and maintained by [Furqan Ahmed](https://github.com/furqan-ahm).
