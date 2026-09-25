/**
 * OrbitLan coordinator — Cloudflare Worker + Durable Object edition.
 *
 * Wire-compatible with the original Go coordinator, so the existing client talks
 * to it by changing ONLY the endpoint URL:
 *
 *   POST /net/join   {code,peerID,name}      -> {netID,yourIP,subnet,members,version,turn?}
 *   POST /net/signal {code,from,to,data}     -> {ok:"1"}
 *   GET  /net/poll?code&peerID&version       -> long-poll {members,messages,version}
 *   POST /net/leave  {code,peerID}           -> {ok:"1"}
 *   GET  /net/health                         -> "ok"
 *
 * Each network (join code) maps to one Durable Object, so state is isolated and
 * scales horizontally at zero cost. Signaling is ephemeral and in-memory: members
 * poll continuously, which keeps their network's DO warm; an emptied network's DO
 * simply evicts, which is the correct state (no members).
 *
 * Relay (the only thing that can cost money) is handed out only when RELAY_ENABLED
 * is "true". Off => direct-only, the free-forever mode. Keep billing OFF on the
 * Cloudflare TURN side and it hard-caps at the free allowance instead of charging.
 */

export interface Env {
  NETWORK: DurableObjectNamespace;
  RELAY_ENABLED: string;      // "true" | "false" — global relay kill-switch (free-pool safety)
  MAX_MEMBERS: string;        // generous per-network cap, e.g. "32"
  CF_TURN_KEY_ID: string;     // secret (wrangler secret put)
  CF_TURN_API_TOKEN: string;  // secret (wrangler secret put)
}

const JSON_HEADERS = { "content-type": "application/json" } as const;
const CODE_RE = /^[A-Za-z0-9_-]{3,64}$/;
const PEER_ID_RE = /^[A-Za-z0-9_-]{1,128}$/;
const MAX_BODY_CHARS = 64_000;
const MAX_NAME_CHARS = 64;
const MAX_INBOX_MESSAGES = 128;
const j = (v: unknown, status = 200) =>
  new Response(JSON.stringify(v), { status, headers: JSON_HEADERS });

// ---- Worker: route every /net/* request to its network's Durable Object ----
export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const url = new URL(req.url);
    const path = url.pathname;

    if (path === "/net/health") return new Response("ok");
    if (!path.startsWith("/net/")) return new Response("not found", { status: 404 });

    // The join code selects the DO. GET carries it in the query; POST in the body.
    let code = url.searchParams.get("code") ?? "";
    let body: string | undefined;
    if (req.method === "POST") {
      const declaredLength = Number(req.headers.get("content-length") ?? "0");
      if (declaredLength > MAX_BODY_CHARS) return new Response("request too large", { status: 413 });
      body = await req.text();
      if (body.length > MAX_BODY_CHARS) return new Response("request too large", { status: 413 });
      if (!code) {
        try {
          const o = JSON.parse(body);
          code = o.code ?? o.Code ?? "";
        } catch { /* leave empty */ }
      }
    }
    if (!CODE_RE.test(code)) return new Response("invalid network code", { status: 400 });

    const stub = env.NETWORK.get(env.NETWORK.idFromName(code));
    // Forward to the DO with the body already buffered so it can re-read it.
    return stub.fetch(new Request(url.toString(), {
      method: req.method,
      headers: req.headers,
      body: req.method === "POST" ? body : undefined,
    }));
  },
};

// ---- Durable Object: one network (one join code) --------------------------
interface SigMsg { from: string; data: unknown; }
interface Member {
  peerID: string;
  name: string;
  ip: string;
  lastSeen: number;
  inbox: SigMsg[];
}

const SUBNET = "10.69.0.0/24";
const POLL_MS = 25_000;   // long-poll hold, matches the Go coordinator
const STALE_MS = 60_000;  // drop members that stopped polling

export class Network {
  private env: Env;
  private members = new Map<string, Member>();
  private usedOct = new Set<number>();
  private version = 0;
  private waiters = new Set<() => void>();

  // cached TURN credential (per network; minted once, reused within its TTL)
  private turnUser = "";
  private turnCred = "";
  private turnExpiry = 0;

  constructor(_state: DurableObjectState, env: Env) {
    this.env = env;
  }

  async fetch(req: Request): Promise<Response> {
    this.reap();
    const path = new URL(req.url).pathname;
    switch (path) {
      case "/net/join":   return this.join(req);
      case "/net/signal": return this.signal(req);
      case "/net/poll":   return this.poll(new URL(req.url));
      case "/net/leave":  return this.leave(req);
      default:            return new Response("not found", { status: 404 });
    }
  }

  // wake every long-poll waiting on this network, and advance the version
  private bump() {
    this.version++;
    for (const w of this.waiters) w();
    this.waiters.clear();
  }

  private assignIP(): string {
    for (let oct = 10; oct <= 250; oct++) {
      if (!this.usedOct.has(oct)) {
        this.usedOct.add(oct);
        return `10.69.0.${oct}`;
      }
    }
    return "";
  }

  private snapshot() {
    return [...this.members.values()].map((m) => ({
      peerID: m.peerID, name: m.name, ip: m.ip,
    }));
  }

  // lazily drop members that stopped polling (cheap; runs at the top of every request)
  private reap() {
    const now = Date.now();
    let changed = false;
    for (const [id, m] of this.members) {
      if (now - m.lastSeen > STALE_MS) {
        const oct = parseInt(m.ip.split(".")[3] ?? "0", 10);
        if (oct) this.usedOct.delete(oct);
        this.members.delete(id);
        changed = true;
      }
    }
    if (changed) this.bump();
  }

  private async join(req: Request): Promise<Response> {
    const b = (await req.json().catch(() => null)) as any;
    const code = b?.code ?? b?.Code;
    const peerID = b?.peerID ?? b?.PeerID;
    const name = b?.name ?? b?.Name ?? "";
    if (!CODE_RE.test(code) || !PEER_ID_RE.test(peerID) ||
        typeof name !== "string" || name.length > MAX_NAME_CHARS) {
      return new Response("bad request", { status: 400 });
    }

    let m = this.members.get(peerID);
    if (!m) {
      const cap = Math.min(250, Math.max(2, parseInt(this.env.MAX_MEMBERS || "32", 10) || 32));
      if (this.members.size >= cap) return new Response("network full", { status: 403 });
      m = { peerID, name, ip: this.assignIP(), lastSeen: Date.now(), inbox: [] };
      this.members.set(peerID, m);
      this.bump();
    }
    m.lastSeen = Date.now();

    const resp: any = {
      netID: "net_" + code,
      yourIP: m.ip,
      subnet: SUBNET,
      members: this.snapshot(),
      version: this.version,
    };
    if ((this.env.RELAY_ENABLED ?? "true") === "true") {
      const t = await this.turn();
      if (t) resp.turn = t;
    }
    return j(resp);
  }

  private async signal(req: Request): Promise<Response> {
    const b = (await req.json().catch(() => null)) as any;
    const to = b?.to ?? b?.To;
    const from = b?.from ?? b?.From ?? "";
    const data = b?.data ?? b?.Data;
    if (!PEER_ID_RE.test(to) || !PEER_ID_RE.test(from) || data === undefined) {
      return new Response("bad request", { status: 400 });
    }
    const m = to ? this.members.get(to) : undefined;
    if (m) {
      if (m.inbox.length >= MAX_INBOX_MESSAGES) m.inbox.shift();
      m.inbox.push({ from, data });
      this.bump();
    }
    return j({ ok: "1" });
  }

  private async poll(url: URL): Promise<Response> {
    const peerID = url.searchParams.get("peerID") ?? "";
    const clientVer = url.searchParams.get("version") ?? "";
    if (!PEER_ID_RE.test(peerID)) return new Response("bad request", { status: 400 });
    let m = this.members.get(peerID);
    if (!m) return new Response("not a member", { status: 410 });
    m.lastSeen = Date.now();

    // Immediate return if there's mail waiting or membership changed since the client's version.
    if (m.inbox.length > 0 || String(this.version) !== clientVer) {
      const msgs = m.inbox; m.inbox = [];
      return j({ members: this.snapshot(), messages: msgs, version: this.version });
    }

    // Otherwise hold the request until something changes or the poll window elapses.
    await new Promise<void>((resolve) => {
      const done = () => { clearTimeout(timer); this.waiters.delete(done); resolve(); };
      const timer = setTimeout(done, POLL_MS);
      this.waiters.add(done);
    });

    m = this.members.get(peerID);
    if (!m) return new Response("not a member", { status: 410 });
    m.lastSeen = Date.now();
    const msgs = m.inbox; m.inbox = [];
    return j({ members: this.snapshot(), messages: msgs, version: this.version });
  }

  private async leave(req: Request): Promise<Response> {
    const b = (await req.json().catch(() => null)) as any;
    const peerID = b?.peerID ?? b?.PeerID;
    if (!PEER_ID_RE.test(peerID)) return new Response("bad request", { status: 400 });
    const m = peerID ? this.members.get(peerID) : undefined;
    if (m) {
      const oct = parseInt(m.ip.split(".")[3] ?? "0", 10);
      if (oct) this.usedOct.delete(oct);
      this.members.delete(peerID);
      this.bump();
    }
    return j({ ok: "1" });
  }

  // turn mints (and caches) a Cloudflare TURN credential for this network.
  // Cached 23h against a 24h TTL, so any client always receives >=1h of validity.
  private async turn(): Promise<{ url: string; username: string; credential: string } | null> {
    const url = "turn:turn.cloudflare.com:3478?transport=udp";
    const now = Date.now();
    if (this.turnUser && now < this.turnExpiry) {
      return { url, username: this.turnUser, credential: this.turnCred };
    }
    const keyID = this.env.CF_TURN_KEY_ID, token = this.env.CF_TURN_API_TOKEN;
    if (!keyID || !token) return null;
    try {
      const r = await fetch(
        `https://rtc.live.cloudflare.com/v1/turn/keys/${keyID}/credentials/generate`,
        {
          method: "POST",
          headers: { authorization: "Bearer " + token, "content-type": "application/json" },
          body: JSON.stringify({ ttl: 86400 }),
        },
      );
      if (!r.ok) return null;
      const o = (await r.json()) as any;
      const u = o?.iceServers?.username, c = o?.iceServers?.credential;
      if (!u || !c) return null;
      this.turnUser = u; this.turnCred = c;
      this.turnExpiry = now + 23 * 60 * 60 * 1000;
      return { url, username: u, credential: c };
    } catch {
      return null;
    }
  }
}
