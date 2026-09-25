# OrbitLan Coordinator (Cloudflare Workers + Durable Objects)

The signaling + TURN-credential coordinator for OrbitLan, running on Cloudflare's
edge. It is **wire-compatible** with the original Go coordinator, so the client
points at it by changing only its endpoint URL.

## Why the edge
- **Latency**: signaling happens at the Cloudflare POP nearest each player (~5 ms),
  instead of a single VM in one region.
- **Scale**: one Durable Object per network (join code) — isolated state, horizontal
  scale, no shared bottleneck.
- **Cost**: designed for the Workers free plan (currently 100,000 Worker and Durable Object
  requests per day, with daily hard limits). The data path never touches it — gameplay is
  peer-to-peer.

## API (unchanged from the Go coordinator)
| Method | Path | Body / Query | Returns |
|---|---|---|---|
| POST | `/net/join` | `{code,peerID,name}` | `{netID,yourIP,subnet,members,version,turn?}` |
| POST | `/net/signal` | `{code,from,to,data}` | `{ok:"1"}` |
| GET | `/net/poll` | `?code&peerID&version` | long-poll `{members,messages,version}` |
| POST | `/net/leave` | `{code,peerID}` | `{ok:"1"}` |
| GET | `/net/health` | — | `ok` |

`turn` is included only when relay is enabled (see below).

## Relay policy (the only thing that can cost money)
`RELAY_ENABLED` (a plain var in `wrangler.toml`) is the global switch for the shared
free relay pool:
- `"true"` (default) — hand out Cloudflare TURN credentials; direct-first, relay
  fallback.
- `"false"` — never hand out TURN; **direct-only for everyone** (the free-forever mode).

**Keep billing OFF on the Cloudflare TURN side** and it hard-caps at the free
allowance (1000 GB/mo) instead of ever charging you — the pool simply stops relaying,
and clients fall back to direct-only. Clients also have their own relay Off/Auto/On
toggle, so a user can force direct-only regardless of this server setting.

## Deploy
```bash
npm install
npx wrangler secret put CF_TURN_KEY_ID      # your Cloudflare Realtime TURN key id
npx wrangler secret put CF_TURN_API_TOKEN    # its API token
npx wrangler deploy
```
The deploy prints your Worker URL (e.g. `https://orbitlan.<subdomain>.workers.dev`).
Point the client's coordinator endpoint at it. Add a custom domain in the Cloudflare
dashboard if you want a stable branded URL.

The official deployment is `https://orbitlan.furqan-ahm.workers.dev`.

## Local development
```bash
cp .dev.vars.example .dev.vars   # fill in your TURN key id + token
npx wrangler dev                 # runs a real local Durable Object via workerd
```
Then exercise it:
```bash
curl -s -X POST localhost:8787/net/join -d '{"code":"demo","peerID":"A","name":"Alice"}'
curl -s "localhost:8787/net/poll?code=demo&peerID=A&version=0"
```

## Self-hosting
This is exactly what a self-hoster deploys to run their own free, unlimited
coordinator. Combined with their own [coturn](https://github.com/coturn/coturn) relay
and the client's configurable endpoints, a community can run OrbitLan entirely on
their own infrastructure at no cost to anyone else.
