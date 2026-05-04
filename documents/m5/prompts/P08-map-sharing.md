# P08 — Map sharing across the network

## What this prompt does

Implements server-to-client streaming of `.lvl` files. Server's `INITIAL_STATE` includes a `MapDescriptor` (CRC32 + short name + size). Client checks local `assets/maps/` then a content-addressed download cache (`<XDG/AppData>/soldut/maps/<crc32>.lvl`). Two new wire messages — `NET_MSG_MAP_REQUEST` and `NET_MSG_MAP_CHUNK` — stream the file at 1180 bytes per packet on `NET_CH_LOBBY` (reliable, ordered). Client validates CRC, writes to cache, loads. Mid-round join handled.

Depends on P01 (.lvl format).

## Required reading

1. `CLAUDE.md`
2. `documents/05-networking.md` §"Connection flow", §"Channels"
3. **`documents/m5/10-map-sharing.md`** — the spec for this prompt
4. `documents/m5/01-lvl-format.md` — file format and CRC validation
5. `src/net.{c,h}` — channels, NET_MSG_* enum, NetPeer, server/client state
6. `src/main.c::bootstrap_host`, `bootstrap_client` — initial-state path
7. `src/level_io.{c,h}` — `level_load`, `LvlResult` (P01)
8. `src/maps.{c,h}` — `map_build` (you'll add map-descriptor lookup)
9. `src/lobby_ui.c` — for the download progress UI

## Background

Without this feature, only the 8 shipped maps can ever be played. A host who authors a custom map in the editor (P04) can't share it.

## Concrete tasks

### Task 1 — `MapDescriptor` in `INITIAL_STATE`

Add to `src/net.h`:

```c
typedef struct MapDescriptor {
    uint32_t crc32;
    uint32_t size_bytes;
    uint8_t  short_name_len;
    char     short_name[24];
    uint8_t  reserved[3];
} MapDescriptor;
```

32 bytes. Append to `NET_MSG_INITIAL_STATE` body.

In server `bootstrap_host`/`start_round` paths, fill the descriptor from the current map's .lvl file: read the CRC32 from the saved file's header, fill short_name from the MapDef.

In client `client_handle_initial_state`, store the descriptor in `g->pending_map`.

### Task 2 — Client-side cache + lookup

`src/map_cache.{h,c}` (new module):

```c
typedef struct {
    char path[512];
    uint32_t crc32;
    uint32_t size_bytes;
    time_t   mtime;
} MapCacheEntry;

void map_cache_init(void);             // ensures cache dir exists
bool map_cache_has(uint32_t crc32);    // looks up <cache>/<crc>.lvl
void map_cache_write(uint32_t crc32, const uint8_t *data, uint32_t size);
const char *map_cache_path(uint32_t crc32);   // returns <cache>/<crc>.lvl
void map_cache_evict_lru(uint64_t cap_bytes); // 64 MB cap
```

Cache directory per `documents/m5/10-map-sharing.md` §"Download state":
- Linux: `$XDG_DATA_HOME/soldut/maps/` (default `~/.local/share/soldut/maps/`)
- macOS: `~/Library/Application Support/Soldut/maps/`
- Windows: `%APPDATA%/Soldut/maps/`

Use raylib's `GetApplicationDirectory` only for the asset-shipped path lookup; the cache uses platform calls (`getenv("XDG_DATA_HOME")` etc., `mkdir -p` via `mkdir(2)` chain).

### Task 3 — `client_has_map` decision

In `src/net.c::client_handle_initial_state`:

```c
const char *client_resolve_map_path(const MapDescriptor *m, char *out, size_t out_cap) {
    char shipped[256];
    snprintf(shipped, sizeof shipped, "assets/maps/%s.lvl", m->short_name);
    if (map_file_crc(shipped) == m->crc32) {
        snprintf(out, out_cap, "%s", shipped);
        return out;
    }
    if (map_cache_has(m->crc32)) {
        snprintf(out, out_cap, "%s", map_cache_path(m->crc32));
        return out;
    }
    return NULL;   // need to download
}
```

If a path is found, `level_load` it directly and send `NET_MSG_MAP_READY` with status 0. Otherwise, begin download.

### Task 4 — Wire protocol

Add to `src/net.h`:

```c
NET_MSG_MAP_REQUEST = 40,    // client → server (NET_CH_LOBBY)
NET_MSG_MAP_CHUNK   = 41,    // server → client (NET_CH_LOBBY)
NET_MSG_MAP_READY   = 42,    // client → server (NET_CH_LOBBY)
```

`MAP_REQUEST`: 9 bytes (msg_type + crc32 + resume_offset).

`MAP_CHUNK`: 16-byte header (msg_type + crc32 + total_size + chunk_offset + chunk_len + is_last + reserved) + payload (≤1180 bytes).

`MAP_READY`: 8 bytes (msg_type + crc32 + status + reserved).

### Task 5 — Server-side request handler

`src/net.c::server_handle_map_request`:

```c
fopen current map .lvl
fseek to resume_offset
loop:
  read up to 1180 bytes
  send NET_MSG_MAP_CHUNK on NET_CH_LOBBY (reliable)
  off += chunk_size
  if off >= total: break
fclose
```

ENet's reliable channel handles backpressure. Don't try to throttle manually.

### Task 6 — Client-side download state + handler

`src/map_download.{h,c}`:

```c
typedef struct MapDownload {
    uint32_t crc32;
    uint32_t total_size;
    uint8_t *buffer;          // total_size bytes, allocated on permanent arena
    uint8_t *received_bits;   // total_size / 8 bits — 1 = received
    uint32_t bytes_received;
    bool     active;
    bool     complete;
} MapDownload;
```

`client_begin_map_download(g, descriptor)` allocates buffer + received_bits, sends `MAP_REQUEST`.

`client_handle_map_chunk(g, chunk)` per `documents/m5/10-map-sharing.md` §"On MAP_CHUNK":
- Validate `c->crc32 == g->map_download.crc32`.
- Validate `c->chunk_offset + c->chunk_len <= total_size`.
- Memcpy chunk data into buffer at offset.
- Update bytes_received + bits.
- On `is_last`: validate full receipt, compute CRC over buffer, compare to `crc32`. On match, write to cache via `map_cache_write`, `level_load`, send `MAP_READY` status=0. On mismatch, status=1; log loudly.

### Task 7 — Lobby UI download progress

In `lobby_screen_run` (`src/lobby_ui.c`), if `g->map_download.active`, show a "Downloading map: NN%" progress panel. Hide other interactive elements during download.

Format: `bytes_received / total_size * 100`. Update every frame (it's just text + a bar; cheap).

### Task 8 — Stragglers + mid-round join

A slow downloader misses round start. When their download completes:

1. `level_load` runs.
2. The match's already-active state (received via `LOBBY_ROUND_START`) means the client's `match.phase == ACTIVE`.
3. The client's per-frame check at top of MODE_MATCH late-binds `world.local_mech_id` (already in M4 main.c).
4. Their mech respawns at next-spawn cycle.

A new client joining mid-round same-shape: download → load → mid-round respawn.

### Task 9 — Trust + validation

Per `documents/m5/10-map-sharing.md` §"Trust model":

- Reject `MapDescriptor.size_bytes > 2 * 1024 * 1024` (2 MB cap).
- Reject MAP_CHUNK with chunk_offset + chunk_len > total_size.
- 30 s stall timeout → abort + drop connection.
- Hard 64 MB cache cap; LRU evict on write.

## Done when

- `make` builds clean.
- A client connecting to a server whose .lvl isn't in the client's `assets/maps/` successfully downloads, validates, caches, and plays.
- Cache directory created with correct platform path.
- Bit-flipped chunk rejected at CRC; client logs and disconnects.
- Cache evicts oldest files when total exceeds 64 MB.
- Mid-round join works on a custom map.
- Lobby UI shows download progress.
- Test scenario `tests/net/run_map_share.sh` (write it as part of this prompt) passes in CI.

## Out of scope

- Sharing assets *other than* the .lvl (custom backgrounds, custom music): defer to v0.2.
- Server-side download throttling for many simultaneous joins: tracked as trade-off, defer.
- Resume-across-server-restart: defer.
- A "map signature" / authenticity check beyond CRC: defer; CRC + parser hardening is sufficient.

## How to verify

```bash
make
./tests/net/run.sh
./tests/net/run_map_share.sh   # new test you write
```

The new test:
1. Synthesize a temp .lvl file with a unique CRC.
2. Start host with that map.
3. Start client without the map locally; clear its cache.
4. Assert: INITIAL_STATE arrives, MAP_REQUEST goes out, chunks stream, MAP_READY status=0, round starts.
5. Inspect cache: `ls $XDG_DATA_HOME/soldut/maps/<crc>.lvl` exists.

## Close-out

1. Update `CURRENT_STATE.md`: map sharing across network.
2. Update `TRADE_OFFS.md`: **add** "Asset references inside a custom .lvl resolve to client-local files" (pre-disclosed). **Add** "No download throttling at the server" (pre-disclosed). **Add** "No download resume across host restarts" (pre-disclosed).
3. Don't commit unless explicitly asked.

## Common pitfalls

- **CRC over the file with the CRC field zeroed**: same as P01's level_io. Reuse the helper.
- **`bytes_received` increments on already-received chunks**: ENet's reliable channel can deliver the same chunk if the sender retransmitted before the ACK landed. Use the `received_bits` to track which chunks are unique.
- **The `permanent` arena for the download buffer**: it must outlive the round (cache-write happens after round-start in some scenarios). Don't use level_arena.
- **Cache directory mkdir -p**: walk the path and create each segment. raylib doesn't help here; use POSIX `mkdir(2)` or Win32 `CreateDirectoryA`.
- **Platform paths**: getenv on Linux can return NULL; default to `$HOME/.local/share`. On Windows, use `%APPDATA%`. Don't hard-code `/home/...`.
