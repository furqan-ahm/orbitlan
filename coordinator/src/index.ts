/**
 * OrbitLan coordinator — Cloudflare Worker + Durable Object edition.
 *
 * Wire-compatible with the original Go coordinator, so the existing client talks
 * to it by changing ONLY the endpoint URL:
 *
 *   POST /net/join   {code,peerID,name,edition} -> {netID,yourIP,subnet,members,version,turn?}
 *   POST /net/signal {code,from,to,data}     -> {ok:"1"}
 *   GET  /net/poll?code&peerID&version       -> long-poll {members,messages,version}
 *   POST /net/leave  {code,peerID}           -> {ok:"1"}
 *   POST /net/kick   {code,from,target,memberToken} -> {ok:"1"}
 *   GET  /net/health                         -> "ok"
 *
 * Each network (join code) maps to one Durable Object, so state is isolated and
 * scales horizontally. Signaling is ephemeral and in-memory: members
 * poll continuously, which keeps their network's DO warm; an emptied network's DO
 * simply evicts, which is the correct state (no members).
 *
 * Relay credentials are handed out only when RELAY_ENABLED is "true". Off means
 * direct-only. Operators remain responsible for their provider's usage and costs.
 */

export interface Env {
  NETWORK: DurableObjectNamespace;
  RELAY_ENABLED: string;      // "true" | "false" — global relay kill-switch (free-pool safety)
  MAX_MEMBERS: string;        // generous per-network cap, e.g. "32"
  CF_TURN_KEY_ID?: string;     // secret (wrangler secret put)
  CF_TURN_API_TOKEN?: string;  // secret (wrangler secret put)
  TURN_URL?: string;           // optional self-hosted coturn URL
  TURN_SHARED_SECRET?: string; // optional coturn REST secret (wrangler secret put)
}

const JSON_HEADERS = { "content-type": "application/json" } as const;
const CODE_RE = /^[A-Za-z0-9_-]{3,64}$/;
const PEER_ID_RE = /^[A-Za-z0-9_-]{1,128}$/;
const MEMBER_TOKEN_RE = /^[a-f0-9]{64}$/i;
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
  tokenHash: string;
  joinedAt: number;
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
  private roomEdition: "community" | "supporter" | "" = "";
  private memberCap = 0;
  private hostPeerID = "";
  private bannedPeerIDs = new Set<string>();

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
      case "/net/poll":   return this.poll(req);
      case "/net/leave":  return this.leave(req);
      case "/net/kick":   return this.kick(req);
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

  private resetRoom() {
    this.usedOct.clear();
    this.roomEdition = "";
    this.memberCap = 0;
    this.hostPeerID = "";
    this.bannedPeerIDs.clear();
  }

  private promoteHost() {
    if (this.hostPeerID && this.members.has(this.hostPeerID)) return;
    let oldest: Member | undefined;
    for (const member of this.members.values()) {
      if (!oldest || member.joinedAt < oldest.joinedAt) oldest = member;
    }
    this.hostPeerID = oldest?.peerID ?? "";
  }

  private removeMember(peerID: string, ban: boolean) {
    const member = this.members.get(peerID);
    if (!member) return false;
    const oct = parseInt(member.ip.split(".")[3] ?? "0", 10);
    if (oct) this.usedOct.delete(oct);
    this.members.delete(peerID);
    if (ban) this.bannedPeerIDs.add(peerID);
    if (this.members.size === 0) this.resetRoom();
    else if (this.hostPeerID === peerID) this.promoteHost();
    return true;
  }

  private async memberAuthenticated(peerID: string, token: string): Promise<boolean> {
    const member = this.members.get(peerID);
    if (!member) return false;
    // Preserve wire compatibility with pre-token clients. New clients are always
    // authenticated, including after an automatic host promotion.
    if (!member.tokenHash) return true;
    return MEMBER_TOKEN_RE.test(token) && member.tokenHash === await this.hashToken(token);
  }

  // lazily drop members that stopped polling (cheap; runs at the top of every request)
  private reap() {
    const now = Date.now();
    let changed = false;
    for (const [id, m] of this.members) {
      if (now - m.lastSeen > STALE_MS) {
        changed = this.removeMember(id, false) || changed;
      }
    }
    if (changed) {
      this.bump();
    }
  }

  private async join(req: Request): Promise<Response> {
    const b = (await req.json().catch(() => null)) as any;
    const code = b?.code ?? b?.Code;
    const peerID = b?.peerID ?? b?.PeerID;
    const name = b?.name ?? b?.Name ?? "";
    const memberToken = b?.memberToken ?? b?.MemberToken ?? "";
    const requestedEdition = (b?.edition ?? b?.Edition) === "supporter"
      ? "supporter" as const
      : "community" as const;
    if (!CODE_RE.test(code) || !PEER_ID_RE.test(peerID) ||
        typeof name !== "string" || name.length > MAX_NAME_CHARS ||
        (memberToken !== "" && !MEMBER_TOKEN_RE.test(memberToken))) {
      return new Response("bad request", { status: 400 });
    }
    if (this.bannedPeerIDs.has(peerID)) {
      return new Response("You were removed from this room by its host.", { status: 403 });
    }
    const tokenHash = memberToken ? await this.hashToken(memberToken) : "";

    let m = this.members.get(peerID);
    if (m && m.tokenHash && m.tokenHash !== tokenHash) {
      return new Response("member identity mismatch", { status: 403 });
    }
    if (!m) {
      const globalCap = Math.min(241, Math.max(2, parseInt(this.env.MAX_MEMBERS || "32", 10) || 32));
      if (this.members.size === 0 || this.memberCap === 0) {
        this.roomEdition = requestedEdition;
        this.memberCap = requestedEdition === "supporter" ? globalCap : Math.min(4, globalCap);
        this.hostPeerID = peerID;
        this.bannedPeerIDs.clear();
      }
      if (this.members.size >= this.memberCap) {
        const message = this.roomEdition === "community"
          ? "This Community-hosted network is limited to 4 nodes. A Supporter host can create larger networks."
          : "This network has reached its coordinator limit.";
        return new Response(message, { status: 403 });
      }
      const now = Date.now();
      m = { peerID, name, ip: this.assignIP(), lastSeen: now, inbox: [], tokenHash, joinedAt: now };
      this.members.set(peerID, m);
      this.bump();
    }
    m.name = name;
    m.lastSeen = Date.now();

    const resp: any = {
      netID: "net_" + code,
      yourIP: m.ip,
      subnet: SUBNET,
      members: this.snapshot(),
      version: this.version,
      roomEdition: this.roomEdition,
      memberCap: this.memberCap,
      hostPeerID: this.hostPeerID,
      isHost: this.hostPeerID === peerID,
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
    if (!await this.memberAuthenticated(from, req.headers.get("x-orbitlan-member") ?? "")) {
      return new Response("member authentication failed", { status: 403 });
    }
    const m = to ? this.members.get(to) : undefined;
    if (m) {
      if (m.inbox.length >= MAX_INBOX_MESSAGES) m.inbox.shift();
      m.inbox.push({ from, data });
      this.bump();
    }
    return j({ ok: "1" });
  }

  private async poll(req: Request): Promise<Response> {
    const url = new URL(req.url);
    const peerID = url.searchParams.get("peerID") ?? "";
    const clientVer = url.searchParams.get("version") ?? "";
    if (!PEER_ID_RE.test(peerID)) return new Response("bad request", { status: 400 });
    let m = this.members.get(peerID);
    if (!m) {
      return new Response(this.bannedPeerIDs.has(peerID)
        ? "You were removed from this room by its host."
        : "not a member", { status: this.bannedPeerIDs.has(peerID) ? 403 : 410 });
    }
    if (!await this.memberAuthenticated(peerID, req.headers.get("x-orbitlan-member") ?? "")) {
      return new Response("member authentication failed", { status: 403 });
    }
    m.lastSeen = Date.now();

    // Immediate return if there's mail waiting or membership changed since the client's version.
    if (m.inbox.length > 0 || String(this.version) !== clientVer) {
      const msgs = m.inbox; m.inbox = [];
      return j({ members: this.snapshot(), messages: msgs, version: this.version,
                 hostPeerID: this.hostPeerID, isHost: this.hostPeerID === peerID });
    }

    // Otherwise hold the request until something changes or the poll window elapses.
    await new Promise<void>((resolve) => {
      const done = () => { clearTimeout(timer); this.waiters.delete(done); resolve(); };
      const timer = setTimeout(done, POLL_MS);
      this.waiters.add(done);
    });

    m = this.members.get(peerID);
    if (!m) {
      return new Response(this.bannedPeerIDs.has(peerID)
        ? "You were removed from this room by its host."
        : "not a member", { status: this.bannedPeerIDs.has(peerID) ? 403 : 410 });
    }
    m.lastSeen = Date.now();
    const msgs = m.inbox; m.inbox = [];
    return j({ members: this.snapshot(), messages: msgs, version: this.version,
               hostPeerID: this.hostPeerID, isHost: this.hostPeerID === peerID });
  }

  private async leave(req: Request): Promise<Response> {
    const b = (await req.json().catch(() => null)) as any;
    const peerID = b?.peerID ?? b?.PeerID;
    if (!PEER_ID_RE.test(peerID)) return new Response("bad request", { status: 400 });
    if (!await this.memberAuthenticated(peerID, req.headers.get("x-orbitlan-member") ?? "")) {
      return new Response("member authentication failed", { status: 403 });
    }
    if (this.removeMember(peerID, false)) this.bump();
    return j({ ok: "1" });
  }

  private async kick(req: Request): Promise<Response> {
    const b = (await req.json().catch(() => null)) as any;
    const from = b?.from ?? b?.From ?? "";
    const target = b?.target ?? b?.Target ?? "";
    const memberToken = b?.memberToken ?? b?.MemberToken ?? "";
    if (!PEER_ID_RE.test(from) || !PEER_ID_RE.test(target) ||
        !MEMBER_TOKEN_RE.test(memberToken) || from === target) {
      return new Response("bad request", { status: 400 });
    }
    if (from !== this.hostPeerID) return new Response("only the room host can remove nodes", { status: 403 });
    const host = this.members.get(from);
    if (!host || !host.tokenHash || host.tokenHash !== await this.hashToken(memberToken)) {
      return new Response("host authentication failed", { status: 403 });
    }
    if (!this.removeMember(target, true)) return new Response("node not found", { status: 404 });
    this.bump();
    return j({ ok: "1" });
  }

  private async hashToken(token: string): Promise<string> {
    const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(token));
    return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
  }

  // turn mints (and caches) a short-lived credential for either a self-hosted
  // coturn relay or Cloudflare Realtime TURN. Cached 23h against a 24h TTL.
  private async turn(): Promise<{ url: string; username: string; credential: string } | null> {
    const now = Date.now();
    if (this.turnUser && now < this.turnExpiry) {
      return { url: this.env.TURN_URL || "turn:turn.cloudflare.com:3478?transport=udp",
               username: this.turnUser, credential: this.turnCred };
    }

    // coturn's REST authentication uses expiry:user as the username and a
    // base64 HMAC-SHA1 of that username as the temporary password.
    const selfHostedURL = this.env.TURN_URL?.trim();
    const sharedSecret = this.env.TURN_SHARED_SECRET;
    if (selfHostedURL && sharedSecret) {
      const username = `${Math.floor(now / 1000) + 86400}:orbitlan`;
      const key = await crypto.subtle.importKey(
        "raw", new TextEncoder().encode(sharedSecret),
        { name: "HMAC", hash: "SHA-1" }, false, ["sign"],
      );
      const signature = new Uint8Array(await crypto.subtle.sign(
        "HMAC", key, new TextEncoder().encode(username),
      ));
      let binary = "";
      for (const byte of signature) binary += String.fromCharCode(byte);
      this.turnUser = username;
      this.turnCred = btoa(binary);
      this.turnExpiry = now + 23 * 60 * 60 * 1000;
      return { url: selfHostedURL, username, credential: this.turnCred };
    }

    const url = "turn:turn.cloudflare.com:3478?transport=udp";
    const keyID = this.env.CF_TURN_KEY_ID, token = this.env.CF_TURN_API_TOKEN;
    if (!keyID || !token) return null;
    try {
      const r = await fetch(
        `https://rtc.live.cloudflare.com/v1/turn/keys/${keyID}/credentials/generate-ice-servers`,
        {
          method: "POST",
          headers: { authorization: "Bearer " + token, "content-type": "application/json" },
          body: JSON.stringify({ ttl: 86400 }),
        },
      );
      if (!r.ok) return null;
      const o = (await r.json()) as any;
      const servers = Array.isArray(o?.iceServers) ? o.iceServers : [o?.iceServers];
      const relay = servers.find((server: any) => server?.username && server?.credential);
      const u = relay?.username, c = relay?.credential;
      if (!u || !c) return null;
      this.turnUser = u; this.turnCred = c;
      this.turnExpiry = now + 23 * 60 * 60 * 1000;
      return { url, username: u, credential: c };
    } catch {
      return null;
    }
  }
}
