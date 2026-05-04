# M5 — Map sharing across the network

A server should be able to load a map the connecting client doesn't have, push it down the wire, and play. This is what every Soldat-tradition shooter does (Soldat itself, Quake 1–3, Source) and it's how community maps spread without anyone having to publish to a central distribution. M5 lights it up.

The format that gets distributed is in [01-lvl-format.md](01-lvl-format.md); the protocol channels are in [src/net.h](../../src/net.h) (existing). This document specifies the two new messages, the cache, the trust model, and the join handshake.

## What the M4 build does today

`bootstrap_host` and `bootstrap_client` in `src/main.c` both expect the map files to **already be present** on disk: the server calls `map_build(MAP_FOUNDRY, ...)` (currently a code-built fallback) and the client receives `INITIAL_STATE` containing only the lobby table — **no map identity is on the wire**. The client implicitly assumes its local `MapId` enum is the same as the server's. Any divergence (server runs a custom map; server runs a future map; client built without a map ID) causes silent state mismatch.

This is fine while every map ships in the binary. It breaks the moment a host wants to play a custom map.

## The shape of the feature

```
Client connects → server's INITIAL_STATE includes
                  current map's CRC32 + short_name.
       ↓
Client checks: do I have assets/maps/<short>.lvl
       AND does its CRC match?
   YES → done; client uses local file.
    NO → client sends NET_MSG_MAP_REQUEST(crc32).
       ↓
Server fragments the .lvl into 1200-byte chunks
   and ships them on NET_CH_LOBBY (reliable, ordered)
   tagged NET_MSG_MAP_CHUNK.
       ↓
Client reassembles, validates CRC, writes to
   the *download cache* (not into assets/maps/),
   loads from cache, signals ready.
       ↓
Round starts.
```

Two new messages on the existing `NET_CH_LOBBY` (reliable, ordered), one new field in `INITIAL_STATE`, and one new directory next to the binary.

## Wire protocol

### `INITIAL_STATE` extension

Currently `INITIAL_STATE` carries the lobby table + match state. M5 adds a `MapDescriptor`:

```c
// New struct on the wire — appended to NET_MSG_INITIAL_STATE body
typedef struct MapDescriptor {
    uint32_t crc32;              // matches the .lvl footer CRC
    uint32_t size_bytes;         // total .lvl size
    uint8_t  short_name_len;
    char     short_name[24];     // null-padded, ASCII
    uint8_t  reserved[3];        // pad to 32 bytes
} MapDescriptor;                  // 32 bytes
```

32 bytes added to every `INITIAL_STATE`. Fired once per join; trivial bandwidth.

### `NET_MSG_MAP_REQUEST` (client → server)

```c
// Tag = NET_MSG_MAP_REQUEST = 40 (next available after 33 = LOBBY_MATCH_STATE)
struct {
    uint8_t  msg_type;
    uint32_t crc32;             // identifies which map; server validates
    uint32_t resume_offset;     // if client has a partial cached download, byte offset to resume from
};                              // 9 bytes
```

`resume_offset` lets a client that disconnected mid-download pick up where it left off on a fresh connect. Set to 0 for fresh requests.

### `NET_MSG_MAP_CHUNK` (server → client)

```c
// Tag = NET_MSG_MAP_CHUNK = 41
struct {
    uint8_t  msg_type;
    uint32_t crc32;             // which map this chunk belongs to (multiplexing safety)
    uint32_t total_size;        // total .lvl size (echoed for validation)
    uint32_t chunk_offset;      // where this chunk starts in the file
    uint16_t chunk_len;         // bytes in this chunk (≤1180)
    uint8_t  is_last;           // 1 = this is the final chunk
    uint8_t  reserved;
    uint8_t  data[];            // variable, chunk_len bytes
};                              // 16 bytes header + payload
```

Per the existing 1200-byte ENet MTU budget ([05-networking.md](../05-networking.md) §"Packet structure"): payload max 1180, plus 16-byte chunk header, plus ENet's own framing → fits cleanly in one MTU. A 500 KB map = ~430 chunks.

ENet's reliable-ordered channel handles retransmission and ordering. We don't reimplement those.

### `NET_MSG_MAP_READY` (client → server)

```c
// Tag = NET_MSG_MAP_READY = 42
struct {
    uint8_t  msg_type;
    uint32_t crc32;             // which map the client is now ready on
    uint8_t  status;            // 0 = ready, 1 = CRC mismatch, 2 = parser failure
    uint8_t  reserved[2];
};                              // 8 bytes
```

After the client has reassembled and validated the file, it sends `MAP_READY`. The server holds round-start until every active peer has sent `MAP_READY` for the current map (or the lobby's `auto_start` fires regardless — see "Stragglers" below).

## Server-side flow

```c
// src/net.c — pseudo
static void server_handle_map_request(NetState *ns, NetPeer *p,
                                       const uint8_t *body, int len)
{
    uint32_t crc      = read_u32(body + 1);
    uint32_t resume   = read_u32(body + 5);
    if (crc != current_map_crc()) {
        // Client requested an old map (stale state). Send the current
        // descriptor back as an unsolicited INITIAL_STATE update.
        net_server_send_initial_state_to(ns, p);
        return;
    }
    // Open the .lvl file and start streaming chunks from `resume`.
    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", current_map_short_name());
    FILE *f = fopen(path, "rb");
    if (!f) {
        // Defensive — server's own map should always exist. Log loudly.
        LOG_E("server_handle_map_request: cannot open %s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    uint32_t total = (uint32_t)ftell(f);
    fseek(f, resume, SEEK_SET);

    uint32_t off = resume;
    while (off < total) {
        uint8_t buf[1200];
        uint32_t want = total - off;
        if (want > 1180) want = 1180;
        size_t got = fread(buf + 16, 1, want, f);
        if (got == 0) break;
        // Header.
        write_u8 (buf + 0, NET_MSG_MAP_CHUNK);
        write_u32(buf + 1, crc);
        write_u32(buf + 5, total);
        write_u32(buf + 9, off);
        write_u16(buf + 13, (uint16_t)got);
        write_u8 (buf + 15, (off + got >= total) ? 1 : 0);
        net_send(p, NET_CH_LOBBY, buf, 16 + (int)got, /*reliable*/true);
        off += (uint32_t)got;
    }
    fclose(f);
}
```

Server pacing: ENet's reliable channel handles backpressure. A 500 KB map is ~430 packets ≈ 600 KB on the wire (with overhead) — at ENet's per-peer reliable throughput (~10 MB/s on a LAN, ~500 KB/s on a typical home upstream), that's well under a second LAN, ~1.5 s WAN. The client sees the lobby UI update with a "Downloading map..." progress bar.

For multi-client downloads (host + N clients all requesting), ENet sends each chunk to each client's peer queue independently. The host's upstream is `chunk_size × clients_requesting × snapshot_rate_concurrent`. We don't pre-stage downloads; if 8 clients all join simultaneously they all request, and the host's upstream briefly spikes. Acceptable for the LAN-first M2 baseline; if a hosted public server sees a burst, we serialize requests — see "Trade-offs" below.

## Client-side flow

### On INITIAL_STATE

```c
// src/net.c — client receives INITIAL_STATE with new MapDescriptor
static void client_handle_initial_state(Game *g, const InitialState *is) {
    // (existing lobby + slot apply)
    g->pending_map = is->map;     // store the descriptor
    if (client_has_map(g, &is->map)) {
        // Local copy matches. Load it.
        char path[256];
        client_resolve_map_path(&is->map, path, sizeof path);
        level_load(&g->world, &g->level_arena, path);
        net_client_send_map_ready(&g->net, is->map.crc32, /*ok*/0);
    } else {
        // Need to download. Spin up the download state.
        client_begin_map_download(g, &is->map);
    }
}
```

`client_has_map` checks two locations in order:

1. `assets/maps/<short_name>.lvl` — the shipped map. CRC32 must match.
2. `<download_cache_dir>/<crc32_hex>.lvl` — a previously-downloaded custom map, named by CRC for content-addressed lookup. Skips name collisions when two servers ship distinct maps with the same `short_name`.

The download cache directory is platform-specific:

| Platform | Path |
|---|---|
| Linux | `$XDG_DATA_HOME/soldut/maps/` (or `~/.local/share/soldut/maps/`) |
| macOS | `~/Library/Application Support/Soldut/maps/` |
| Windows | `%APPDATA%/Soldut/maps/` |

Created on first download. Cache size is capped at **64 MB** (LRU eviction by file mtime). Cap is conservative — a 500 KB map × 64 MB = 128 cached maps; comfortable.

### Download state

```c
// src/map_download.h — new module
typedef struct MapDownload {
    uint32_t crc32;
    uint32_t total_size;
    uint8_t *buffer;           // total_size bytes, allocated on level_arena? No —
                               // permanent arena, since download survives across rounds
    uint8_t *received;          // total_size / 8 bits — 1 = chunk received
    uint32_t bytes_received;
    bool     active;
    bool     complete;
} MapDownload;
```

The download spans potentially the whole lobby phase (a 500 KB map at WAN download speed is ~1 s; 5 MB caps would still finish in seconds). The buffer lives in the permanent arena because it can outlive a level reset.

### On MAP_CHUNK

```c
static void client_handle_map_chunk(Game *g, const MapChunk *c) {
    if (!g->map_download.active || g->map_download.crc32 != c->crc32) {
        // Stale / unsolicited; ignore.
        return;
    }
    if (c->chunk_offset + c->chunk_len > g->map_download.total_size) {
        // Out of bounds — server bug or malicious. Cancel.
        client_cancel_map_download(g);
        return;
    }
    memcpy(g->map_download.buffer + c->chunk_offset, c->data, c->chunk_len);
    g->map_download.bytes_received += c->chunk_len;
    set_received_bit(&g->map_download, c->chunk_offset, c->chunk_len);

    if (c->is_last && g->map_download.bytes_received == g->map_download.total_size) {
        client_finalize_map_download(g);
    }
}
```

`client_finalize_map_download`:

1. Compute CRC32 over the assembled buffer (header CRC field zeroed per [01-lvl-format.md](01-lvl-format.md)).
2. Compare against `crc32` field. Mismatch = `NET_MSG_MAP_READY` with `status = 1`; clear cache; log loudly. (Almost certainly a bug; ENet's reliable channel shouldn't deliver corrupt bytes.)
3. Write the buffer to `<cache>/<crc32_hex>.lvl`.
4. Run `level_load` on the path. If the loader rejects (parser failure), `MAP_READY` with `status = 2`; log; tear down. The level format spec covers parser hardening; defensive parsing lives there.
5. Send `MAP_READY` with `status = 0`.

### Progress UI

The lobby UI gains a small "Downloading map: NN%" panel that's visible during MATCH_PHASE_LOBBY when `map_download.active`. The progress is `bytes_received / total_size`. ~30 LOC in `lobby_ui.c`.

## Trust model

The server is *partially* trusted: it picks the map, but a malicious or buggy server can't crash the client through the map file because:

1. **CRC32 check** rejects any bit-corrupted file. ENet's reliable transport plus this CRC means corrupted-in-transit is impossible to act on.
2. **Total-size cap** — `MapDescriptor.size_bytes` is rejected if > **2 MB**. The largest legitimate map (Citadel) is <100 KB; the cap is conservative to allow growth without enabling memory blow.
3. **Defensive parser** — the loader in [01-lvl-format.md](01-lvl-format.md) §"Validation" rejects files with corrupt internal layout, OOB references, or unreasonable section sizes. Parser hardening is the load-bearing trust check.
4. **Download cap** — total cache size capped at 64 MB; any single map at 2 MB; a malicious server can't fill your disk.
5. **No code execution from .lvl** — the file is data only. There is no scripting, no embedded shaders, no native code. (Per the architecture canon.)

Threats not mitigated:
- **Slow-loris** — a server that drops chunks mid-download wastes the client's lobby time. Cap: if a download stalls (no progress for 30 s), abort + drop connection.
- **Wrong-map maps** — a server can ship a `.lvl` that's technically valid but plays badly (no spawns, deadly tiles everywhere, etc.). Out of scope: the player's recourse is to leave that server.
- **Asset-not-asset** — the `.lvl` file references string-table-indexed asset paths (background PNG, music OGG); the client uses local versions of those. A custom map that references `"music/citadel.ogg"` plays the local citadel.ogg, even if the server has a different one. **Map-shared assets beyond the .lvl** are out of scope at v1; this is documented as a trade-off.

## Stragglers

If one player in a 4-player lobby can't download fast enough, the lobby's `auto_start` timer fires anyway. Behavior:

- The slow client misses round start. The server emits `LOBBY_ROUND_START` to everyone; the slow client's `MapDownload` is still active.
- When the download finishes, the client sees its `match.phase == ACTIVE` and a snapshot stream coming in but the level isn't loaded. The post-finalize path runs `level_load`, then late-binds `world.local_mech_id` from the lobby slot, and the client joins live.
- The slow client's mech doesn't exist on the server until they load the map (because the server's `lobby_spawn_round_mechs` had no client to drive the input). They join *as if* on a mid-round spawn — 3-second respawn countdown, picked spawn point.

This is the right shape: the round doesn't wait for slow downloaders, but slow downloaders aren't excluded from the match.

## Mid-round join

Same shape as stragglers. A new client joining mid-round:

1. `INITIAL_STATE` carries the current map descriptor.
2. If the client doesn't have it, download begins.
3. While downloading, the client sees the lobby progress UI but no rounds.
4. When download finishes, `level_load` runs and the client immediately enters `MATCH_PHASE_ACTIVE` if the round is still running, plus the 3-second-respawn flow.

## Tests

`tests/net/run_map_share.sh` — new test scenario:

1. Start a host with a non-default map (synthesized via `tests/fixtures/synth_map.lvl`).
2. Connect a client that doesn't have that map locally.
3. Assert: client receives `INITIAL_STATE` with the descriptor; client requests; client receives all chunks; client validates CRC; client sends `MAP_READY`; server emits `LOBBY_ROUND_START`; round runs end-to-end.
4. Inspect the client's cache directory has the file at `<crc>.lvl`.

Plus a corruption test: simulate a chunk with bad CRC; assert client rejects and logs.

## Performance and bandwidth

- Per-join cost: ~600 KB on the wire (including ENet overhead) for a max-size 500 KB map. ~1 s LAN, ~3 s WAN.
- Per-mid-round-join cost: same.
- Steady state (no joins): zero. Map-share traffic is event-only.
- Disk: cache capped at 64 MB.
- Memory: the download buffer is `total_size` allocated on permanent arena; reused across rounds. Single allocation.

## Versioning interaction

`SOLDUT_PROTOCOL_ID` doesn't bump for this feature alone; the wire format is additive. We *do* bump on M5 anyway because of the EntitySnapshot widening for powerups + grapple state (covered in [04-pickups.md](04-pickups.md) and [05-grapple.md](05-grapple.md)) — `S0LF` (M4) → `S0LG` (M5). Map sharing rides under that bump.

If a future version revises the `.lvl` format, the *file's* version field (in its header) is what determines compatibility, not the network protocol ID. A v0.2 client running v0.2 code can refuse to load a server's v0.3 `.lvl` and disconnect with a clear error.

## Done when

- A client connecting to a server that has a `.lvl` not in the client's `assets/maps/` successfully downloads, validates, caches, and plays.
- A bit-flipped chunk is rejected at CRC; the client logs and disconnects rather than crashing.
- The cache directory (XDG/AppData/Application Support) is created automatically; capped at 64 MB; LRU-evicted.
- Mid-round join works end-to-end on a custom map.
- The lobby UI shows download progress.
- A `tests/net/run_map_share.sh` scenario passes in CI.

## Trade-offs to log

- **Asset references inside a custom .lvl resolve to client-local files.** A server distributing a custom map with custom music + background PNGs can't share those at v1; only the `.lvl` itself ships. Documented; v0.2 may add a multi-file "map bundle" download.
- **No download throttling at the server.** A burst of 8 simultaneous joins all requesting the same map can briefly saturate the host upstream. Mitigation if it bites: serialize via a queue (one in-flight download per peer, others wait).
- **No download resume across host process restarts.** If a download is mid-flight when the host restarts, the client's `resume_offset` stops being valid (the server's CRC may have changed). The client retries from 0.
- **Cache directory is content-addressed by CRC**, not by short_name. Two servers shipping `foundry.lvl` with different content correctly cache as different CRC files; player sees them as the same `short_name` but with different downloaded paths.
