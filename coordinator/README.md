# OrbitLan Coordinator (Cloudflare Workers + Durable Objects)

The signaling + TURN-credential coordinator for OrbitLan, running on Cloudflare's
edge. It is **wire-compatible** with the original Go coordinator, so the client
points at it by changing only its endpoint URL.

## Why the edge
- **Latency**: signaling happens at the Cloudflare POP nearest each player (~5 ms),
  instead of a single VM in one region.
- **Scale**: one Durable Object per network (join code) — isolated state, horizontal
  scale, no shared bottleneck.
- **Cost**: designed for the Workers free plan. Check Cloudflare's current Workers and
  Durable Objects limits before deployment. The data path never touches the coordinator —
  gameplay is peer-to-peer unless a TURN relay is needed.

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
`RELAY_ENABLED` (a plain var in `wrangler.toml`) is the global relay switch:
- `"true"` (default) — hand out short-lived Cloudflare TURN credentials, or credentials
  for a configured self-hosted coturn server; direct-first, relay fallback.
- `"false"` — never hand out TURN; **direct-only for everyone** (the free-forever mode).

Cloudflare Realtime TURN currently includes a shared monthly free allowance and charges
for egress beyond it. Confirm the current pricing and configure your account's spending
controls before enabling it. Clients also have their own relay Off/Auto/On toggle, so a
user can force direct-only regardless of this server setting.

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

For direct-only operation, set `RELAY_ENABLED = "false"` and deploy without TURN
secrets. To use a self-hosted coturn relay instead, set `TURN_URL` in `wrangler.toml`,
store its REST authentication secret with `npx wrangler secret put TURN_SHARED_SECRET`,
and redeploy. `TURN_URL` takes priority when both relay providers are configured.

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
This is what a self-hoster deploys to run their own coordinator. It can operate
direct-only, use Cloudflare Realtime TURN, or issue short-lived credentials for a
self-hosted [coturn](https://github.com/coturn/coturn) relay. The current coordinator
depends on Cloudflare Durable Objects; an Oracle VM can host coturn, but is not a
drop-in replacement for the coordinator. See the complete
[build and self-hosting guide](https://orbitlan.site/guides/self-host-orbitlan/).
