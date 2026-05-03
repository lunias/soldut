# 05 — Networking

This document specifies how 32 players play together. The architecture is the standard for fast online action games — **authoritative server + client-side prediction + server reconciliation + snapshot interpolation + lag compensation**. We use ENet as the transport. We do *not* aim for determinism. We do *not* attempt kernel-level anti-cheat.

## Topology

```
                     ┌────────────────┐
                     │   Host (P0)    │
                     │ Server + Client │ ← runs both: simulation + render
                     └───────┬────────┘
                             │ UDP
       ┌──────────────┬──────┴──────┬──────────────┐
       │              │             │              │
   ┌───┴───┐      ┌───┴───┐    ┌───┴───┐      ┌────┴───┐
   │ Player│      │ Player│    │ Player│ ...  │ Player │
   │   1   │      │   2   │    │   3   │      │   31   │
   └───────┘      └───────┘    └───────┘      └────────┘
```

The host runs **both** the authoritative simulation and a client. There is no separate dedicated-server binary at v1 (a future stretch goal). The host's client renders its predicted local state and the interpolated remote-player state, like any other client.

## The five pieces

### 1. Authoritative server

The server **owns the world state.** Clients send only inputs (button bitmask, aim angle, sequence number, dt). The server runs the simulation and broadcasts snapshots. **Client cannot mutate health, position, ammo, kills, or any other game-state field.**

This single design decision eliminates an entire class of cheats (memory-edit health, set position, infinite ammo) for free. We commit to it absolutely. No client RPC writes to the server's authoritative state without a sanity check on the server.

### 2. Client-side prediction

The local client runs the **same simulation step** the server does, applied to the local mech only, on every input tick. The result: zero perceived input latency. You move, the screen moves, before the server even knows.

Each input is tagged with a monotonic 16-bit sequence number:

```c
typedef struct {
    uint16_t seq;        // monotonic, wraps
    float    aim_x, aim_y;
    uint16_t buttons;    // bit per button: FIRE, JUMP, JET, LEFT, RIGHT, CROUCH, RELOAD, MELEE, USE, SWAP, ...
    float    dt;
} ClientInput;
```

The client keeps a **circular buffer of unacked inputs** (last 60). When the server snapshot arrives, the client knows which inputs the server has processed.

### 3. Server reconciliation

When a server snapshot arrives, the client reads `last_processed_input_seq` for itself and compares its predicted state to the snapshot. If they match (within a small epsilon), nothing happens.

If they don't match — packet loss, server-side correction, etc. — the client **rewinds** its local mech to the server's authoritative state, then **replays all inputs after `last_processed_input_seq`** using the same simulation function. The visible local mech is *smoothly blended* from the old predicted position to the corrected one over ~100 ms (so corrections aren't visually jarring).

```c
void client_reconcile(Client *c, ServerSnapshot *snap) {
    Mech *me = &c->world.mechs[c->local_id];
    *me = snap->mechs[c->local_id];           // accept server truth
    for (int i = 0; i < c->input_buf.count; ++i) {
        ClientInput *in = &c->input_buf.items[i];
        if (in->seq <= snap->last_input_seq) continue;
        simulate_one_mech(me, in, in->dt);    // replay
    }
    // The visual position is smoothed toward `me->pos` over ~6 frames.
    c->visual_smoothing.target = me->pos;
}
```

### 4. Snapshot interpolation (for *other* players)

Remote mechs are *not* extrapolated. They are rendered **~100 ms in the past**, between the two most recent received snapshots, linearly interpolated:

```c
float t = (render_time - snap_a.server_time) / (snap_b.server_time - snap_a.server_time);
remote_mech.pos = lerp(snap_a.pos, snap_b.pos, t);
```

This hides packet jitter completely. The cost is a small, fixed ping addition for shooting at remote players — accounted for by lag compensation (next).

When a packet drops, we fall back to one tick of dead-reckoning (extrapolate using last-known velocity) before showing a "lost connection" indicator. We do not extrapolate physics-rich motion (collision, jet exhaust trails) — we let it briefly freeze and resume.

### 5. Lag compensation

When a client fires (hitscan), the server doesn't just check the *current* world state. It **rewinds** every other player's hitbox state to the server tick the shooter saw — `server_time - shooter_rtt/2 - interp_delay` — and runs the hit test there. If the rewound test connects, damage applies.

This is what makes "I clearly hit them" feel correct. It also means shots against rapidly-moving targets at high ping still resolve fairly. Rewind window: capped at 200 ms (anything beyond that is treated as too laggy to compensate; the shot misses).

The server stores a rolling history of mech bone positions for the last 12 ticks (200 ms at 60 Hz) per mech. Rewind is a lookup, not a recomputation.

## Tick rates

| Subsystem | Rate | Why |
|---|---|---|
| Simulation (server + client predict) | **60 Hz** | Crisp action feel; matches Soldat tradition |
| Physics sub-step | **120 Hz** internal | Stability for Verlet + constraints, kills tunneling |
| Client input upload | **60 Hz** | Match simulation rate |
| Server snapshot broadcast | **30 Hz** (configurable 20–60) | Halves downstream bandwidth; interp hides it |
| Render | uncapped | Free-running with frame interpolation |

Default snapshot rate is 30 Hz to keep the host's upstream bandwidth manageable for 32 players. LAN games can bump to 60 Hz.

## Packet structure

Each UDP datagram carries our own header:

```c
typedef struct {
    uint16_t protocol_id;     // 0xS0LD - magic, version-bumped on incompat changes
    uint16_t local_seq;       // packet sequence number
    uint16_t remote_seq;      // last seq we received from peer
    uint32_t ack_bitfield;    // 32 bits: 1 = received, position relative to remote_seq
    uint8_t  channel;         // STATE | EVENT | CHAT | LOBBY
    uint8_t  flags;           // RELIABLE, FRAGMENT, ENCRYPTED
    uint16_t payload_len;
} PacketHeader;  // 14 bytes
```

`local_seq`, `remote_seq`, and `ack_bitfield` together implement Glenn Fiedler's 33-packet acknowledgement scheme (each packet acks the last 33 by piggyback). This is what ENet does internally for its reliable channels and what we mirror at the application layer for our own retransmit semantics.

**MTU budget**: 1200 bytes payload per packet. Anything larger is fragmented.

## Channels

ENet exposes channels; we use four:

| Channel | Reliability | Use |
|---|---|---|
| `STATE` (0) | unreliable, sequenced | World snapshots — drop is fine, newer supersedes |
| `EVENT` (1) | reliable, ordered | Kill events, weapon swaps, item pickups |
| `CHAT` (2) | reliable, ordered | Lobby chat, in-game text |
| `LOBBY` (3) | reliable, ordered | Ready-up, team change, map vote, kicks |

Snapshots are unreliable on purpose. A stale snapshot is worse than no snapshot — the next one is coming in 33 ms. Game events (kills, scores, item pickups) must be reliable because missing them desyncs HUD/score.

## Snapshot encoding

A snapshot is the world state at server time `T`. Each mech contributes:

```c
typedef struct {
    uint16_t mech_id;          // 2
    int16_t  pos_x_q, pos_y_q; // 4   (quantized to 1/8 px)
    int16_t  vel_x_q, vel_y_q; // 4   (quantized)
    uint16_t aim_q;            // 2   (uint16 → 0..2π)
    uint16_t torso_q;          // 2
    uint8_t  health;           // 1
    uint8_t  armor;            // 1
    uint8_t  weapon_id;        // 1
    uint8_t  ammo;             // 1
    uint8_t  state_bits;       // 1   (alive, jet, crouch, prone, fire, reload, ...)
    uint8_t  team;             // 1
    // limb HP & detached flags packed
    uint16_t limb_bits;        // 2
} EntitySnapshot;              // 22 bytes
```

22 bytes per entity, **uncompressed**. With 32 entities + 8 bytes snapshot header = 712 bytes. At 30 Hz that's **21 KB/s = 168 kbps** per client downstream. The host upstream at 31 clients is 31× that = **5.2 Mbps** uncompressed.

That's too much for many home uplinks at 60 Hz snapshots. The fix:

1. **Delta encode** against the latest snapshot the client has acked. Idle mechs cost ~0 bytes. A 32-bit "fields-changed" mask + only-changed fields.
2. **Quantize aggressively**. Positions to 1/8 px (16-bit covers ±4096 px world). Velocities to 1/16 unit. Angles to 16-bit fractions of 2π.
3. **Sort by priority**. Fill MTU with the highest-priority entities; lower-priority entities accumulate priority and are guaranteed to be sent within ~3 snapshots.

Realistic per-client downstream after compression: **~6–10 KB/s = 50–80 kbps**. Host upstream: **~1.6 Mbps at 30 Hz × 31 clients**. This fits a typical home connection.

## Connection flow

```
Client                                    Server
  │                                          │
  ├─── CONNECT_REQUEST(version, name) ──────→│
  │                                          │ check version, capacity
  │←── CHALLENGE(nonce) ─────────────────────┤
  ├─── CHALLENGE_RESPONSE(nonce_signed) ────→│
  │                                          │
  │←── ACCEPT(client_id, server_time) ───────┤
  ├─── ACK(client_id) ──────────────────────→│
  │                                          │
  │←── INITIAL_STATE(map_hash, players) ─────┤
  │                                          │
  │←── (entering lobby) ─────────────────────┤
```

Challenge nonce prevents trivial source-IP spoofing. We sign it with a connection token so an attacker can't replay other clients' join handshakes. We do **not** ship asymmetric crypto at v1 — the nonce is HMAC-SHA256 with a per-server secret. Good enough.

## NAT and the IP+port join model

We expect the host to **port-forward UDP** to their machine, default port **23073** (Soldat homage). This is documented in the connect dialog. For users who can't port-forward we offer:

1. **LAN broadcast** — for same-network play. Client broadcasts a `WHO_IS_THERE` to `255.255.255.255:23073`; servers respond with name+map+player count. Zero config.
2. **Optional master server (post-v1)** — a tiny HTTP service that hosts can register with. The server browser pulls a list. Master server can also act as a STUN-style rendezvous for hole-punching, which works for ~70–80% of NAT pairs.

**No relay infrastructure at v1.** No Steam Datagram Relay equivalent. If hole-punching fails for a client, they need a port-forward path or a host on a more permissive network. Document this clearly in the UI.

## Lobby protocol

Reliable channel `LOBBY` carries:

```c
typedef enum {
    LOBBY_PLAYER_LIST,       // server → client, full list
    LOBBY_PLAYER_JOINED,     // server → client, delta
    LOBBY_PLAYER_LEFT,       // server → client, delta
    LOBBY_TEAM_CHANGE,       // bidirectional
    LOBBY_LOADOUT,           // client → server (pick mech, weapons, etc.)
    LOBBY_READY,             // client → server
    LOBBY_READY_STATE,       // server → client (full ready bitmask)
    LOBBY_MAP_VOTE,          // bidirectional
    LOBBY_MAP_VOTE_RESULT,   // server → client
    LOBBY_KICK,              // host → server → all
    LOBBY_BAN,               // host → server (persists in bans.txt)
    LOBBY_CHAT,              // bidirectional, server forwards
    LOBBY_START_COUNTDOWN,   // server → client
    LOBBY_ROUND_START,       // server → client (transitions to in-match)
    LOBBY_ROUND_END,         // server → client (transitions back)
} LobbyMsgType;
```

Server is authoritative for ready state, team membership, and host-only commands. Chat is server-forwarded (not peer-to-peer) for moderation and rate-limiting.

## Determinism

We use floats. Cross-platform float determinism is hard (FMA, x87 vs SSE, transcendentals). With snapshot interpolation we don't need it — clients render server state, they don't reproduce it.

We **do** structure the simulation as `simulate(world, inputs, dt)` with no global RNG, no wall-clock reads, and a seeded PCG random. This gets us:

- Replays (server logs inputs + RNG seed; client replays them).
- Easier testing (deterministic on the same machine).
- Future-proofing toward a rollback-style architecture if we ever decide to scale down to 4-player ranked play.

## Cheating: what we get for free, what we don't fight

**Free wins** (authoritative server):
- Health, ammo, position cannot be client-mutated.
- Speed hacks: server clamps movement per input; over-budget inputs are rejected.
- Teleport hacks: server validates max delta-per-tick.
- Wallhack via packet injection: every snapshot is sequenced and validated.

**Hopeless without major investment** (we accept this):
- **Aimbots** — the client legitimately processes its own aim; statistical detection is a research project, not a feature.
- **ESP / wallhacks (rendering-side)** — best mitigation is **server-side entity culling**: don't send entities the player cannot see (off-screen + small buffer). We implement this — saves bandwidth too.
- **Triggerbots** — input-level automation; only kernel anti-cheat catches reliably.

**Pragmatic stance**:
- HMAC handshake stops trivial spoofing.
- Server-side input sanity stops trivial speed/teleport.
- Aggressive entity culling stops most rendering-side cheats.
- Hosts can `/kick` and `/ban`. Bans persist in `bans.txt` next to the executable.

That's our anti-cheat surface. We do not pursue more. (See [00-vision.md](00-vision.md) — we are not a competitive esport.)

## Library: ENet, with a clean swap interface

We use **ENet** v1. Reasons:
- Pure C (matches our codebase).
- ~5k LOC, single library.
- MIT-licensed.
- Channels with optional reliability+ordering, sequencing, fragmentation, MTU detection.
- Shipped in many indies (Cube, Sauerbraten).

Wrap ENet behind `src/net.h`:

```c
typedef struct NetHost NetHost;
typedef struct NetPeer NetPeer;

bool   net_init(void);
NetHost *net_host_create(uint16_t port, int max_peers);  // server
NetHost *net_client_create(void);                        // client
NetPeer *net_connect(NetHost *h, const char *addr, uint16_t port);
void     net_send(NetPeer *p, int channel, const void *data, int len, int flags);
int      net_poll(NetHost *h, NetEvent *out, int max);   // 0..max events
void     net_disconnect(NetPeer *p);
void     net_destroy(NetHost *h);
```

When/if we outgrow ENet, the swap target is **Valve GameNetworkingSockets** (BSD-3, AES-GCM encryption, NAT traversal). The above interface is small enough to swap behind. We do not start with GNS because:
- C++ codebase forces us to compile a C++ TU.
- Heavier dependency (protobuf, possibly OpenSSL).
- We don't need encryption-by-default at v1; HMAC handshake is sufficient.

We do not consider:
- **yojimbo** — C++, opinionated, low-maintenance.
- **libuv + custom** — would take 6 months to rebuild ENet.
- **Raw sockets** — ditto, plus platform abstraction headache.

## Server browser

UI shows a list of LAN-discovered servers + (optional) master-server-published internet servers. Each row:

```
[ Server name ]  [ map ]  [ mode ]  [  9/16 ]  [ 87 ms ]  [→ Connect]
```

Refresh on demand or every 5 s in the background. "Direct Connect" button takes IP:port for hosts not in the browser.

## Disconnect / reconnect

- Connection timeout: 10 seconds of no traffic.
- Graceful disconnect: client sends `DISCONNECT` reliably; server removes player; remaining players see `LOBBY_PLAYER_LEFT`.
- Crash / hard timeout: same outcome but ungraceful — server detects via timeout.
- **Reconnect: not at v1.** If you drop, you rejoin via the lobby, get a new client_id, possibly land at the next round boundary. (Mid-round rejoin is a stretch goal.)

## Bandwidth target

Per-client downstream: **< 80 kbps** typical, **< 256 kbps** burst.
Host upstream: **< 2 Mbps** at 32 clients, 30 Hz snapshots.

If we exceed: profile, then in priority order — increase delta-encoding effectiveness, reduce snapshot rate, tighter quantization, smaller priority budget per snapshot.

## What we are NOT doing

- **No server-side scripting.** Game logic is C, compiled. No Lua, no Wren.
- **No host migration.** Host quits → match ends. Lobby flow makes a new game easy.
- **No matchmaking pools, ranked, MMR.** It's a server browser.
- **No replay infrastructure** at v1 (server can log inputs; tools come later).
- **No voice chat.** Out of scope.
- **No mid-round join.** You join between rounds. Stretch goal: spectator-on-join.
- **No determinism work.** See above.

## References

- Glenn Fiedler — [gafferongames.com](https://gafferongames.com): *What Every Programmer Needs to Know About Game Networking*, *Networking for Game Programmers* series, *Snapshot Interpolation*, *Snapshot Compression*, *State Synchronization*, *Reliability over UDP*.
- Yahn Bernier (Valve) — *Latency Compensating Methods in Client/Server In-game Protocol Design and Optimization*.
- Valve — *Source Multiplayer Networking* (Source dev wiki).
- Tim Ford & Dan Reed (Blizzard) — *Overwatch Gameplay Architecture and Netcode*, GDC 2017.
- Tribes Networking Model — Frohnmayer & Gift.
- Quake 3 networking — Fabien Sanglard's writeup.
- Riot — *Peeking Behind the Curtains of Valorant's Netcode*.
- ENet: [enet.bespin.org](http://enet.bespin.org).
- Valve GameNetworkingSockets: [github.com/ValveSoftware/GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets).
- netcode.io / reliable.io / yojimbo: [github.com/networkprotocol](https://github.com/networkprotocol).
- Gabriel Gambetta — *Fast-Paced Multiplayer* series.

See [reference/sources.md](reference/sources.md) for the full URL list.
