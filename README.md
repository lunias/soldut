# Soldut

A 2D side-scrolling multiplayer mech shooter in C, in the lineage of Soldat.

The project is in **M5 — Maps & content** (in progress; P01–P08 in,
P09 next). M4 shipped the lobby & matches layer: full game flow
(title → server browser → lobby → countdown → match → summary →
next round), FFA + TDM + CTF modes, per-slot loadout picker,
ready/team toggles, LAN-broadcast server discovery, `soldut.cfg`
for port + match defaults + map/mode rotations, four code-built
maps (Foundry / Slipstream / Reactor / Crossfire). Built on top of **M3**
(combat depth: 5 chassis, 8 primaries + 6 secondaries, projectile
pool, explosions, per-limb HP + dismemberment, recoil + bink +
self-bink, friendly-fire), **M2** (authoritative server / client
prediction + reconciliation / hitscan lag comp / blood + decal
sync), and **M1** (Verlet skeleton physics + ragdoll). M5 so far:
**P01** — `.lvl` binary format + loader/saver
(`src/level_io.{c,h}`); **P02** — polygon collision + slope physics
+ slope-aware post-physics anchor; **P03** — render-side
accumulator + interp alpha + reconcile smoothing + two-snapshot
remote-mech interp + hit/fire event sync; **P04** — standalone
level editor (`tools/editor/`, raygui-based) + game-side
`--test-play <path>` flag + test-play loadout GUI modal (`L` key);
**P05** — pickup runtime (HEALTH / AMMO / ARMOR / WEAPON /
POWERUP / JET_FUEL), Engineer's deployable repair pack, Burst SMG
70 ms cadence, practice-dummy spawner kind, and active-powerup
HUD pill; **P06** — Grappling Hook (one-sided "Tarzan" rope,
hold W to retract, BTN_FIRE chain-refire while attached, BTN_USE
to release; rope length capped at 300 px for tight swings); **P07**
— CTF mode (`src/ctf.{c,h}`, both-flags-home capture rule, 36 px
touch / 30 s auto-return, half-jet + secondary-disabled carrier
penalties, drop-on-death, `NET_MSG_FLAG_STATE` event protocol,
team-colored staff-and-pennant render with off-screen HUD compass);
**P08** — Map sharing across the network (`src/map_cache.{c,h}` +
`src/map_download.{c,h}`, `MapDescriptor` in `INITIAL_STATE`,
`NET_MSG_MAP_REQUEST/CHUNK/READY/DESCRIPTOR`, content-addressed
cache at `<XDG/AppData>/soldut/maps/<crc>.lvl` with 64 MB LRU,
2 MB descriptor cap + 30 s stall watchdog, lobby UI download
progress, host gate on per-peer MAP_READY).
Controls + sprite atlases + audio + 8 authored maps land in P09–P18.
See [documents/11-roadmap.md](documents/11-roadmap.md) and
[documents/m5/](documents/m5/).

For the *current behavior* of the build (vs the design intent in
[documents/](documents/)) see [CURRENT_STATE.md](CURRENT_STATE.md) and
[TRADE_OFFS.md](TRADE_OFFS.md).

## Quick start

Three commands on a fresh Linux checkout:

```bash
sudo apt install -y build-essential libgl1-mesa-dev \
    libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libasound2-dev
make
./soldut
```

A 1280×720 window opens at the **title screen** with five entries:
Single Player, Host Server, Browse Servers (LAN), Direct Connect,
Quit. Pick one and the lobby flow takes over — pick chassis +
loadout + team, hit Ready, the round starts on a 3-second countdown
once everyone is ready (or on a 60s auto-start once half the slots
are filled).

Controls:

| Key            | Action |
|----------------|--------|
| `A` / `D`      | Run left / right |
| `W`            | Jet (consumes fuel); also retracts the grapple rope while attached |
| `Space`        | Jump (when grounded) |
| `Shift`        | Dash (Scout chassis) / Burst-jet (Burst Jet module) |
| `Ctrl` / `S`   | Crouch (drops through ONE_WAY platforms) |
| `X`            | Prone |
| `R`            | Reload |
| `Q`            | Swap primary / secondary weapon |
| `F`            | Melee swing |
| `E`            | Use ability (Engineer pack drop / release grapple rope) |
| `Mouse`        | Aim |
| `Left Mouse`   | Fire (re-press while grappled chains a new grapple) |

(Right Mouse is currently unbound; `BTN_FIRE_SECONDARY` lands at P09.)

Hit the dummy enough and a) it ragdolls, b) limbs come off as their
per-limb HP drops to zero (head, both arms, both legs).

### Loadout & networking flags

CLI flags skip the title screen and pre-fill the lobby loadout:

```bash
# Single-player with a Heavy chassis carrying a Mass Driver:
./soldut --chassis Heavy --primary "Mass Driver" --armor Heavy --jetpack Standard

# Host a LAN server on default port 23073:
./soldut --host

# Connect to a host:
./soldut --connect 192.168.1.42:23073 --name "scout-1" --chassis Scout --primary "Plasma SMG"

# Tournament-style friendly-fire on:
./soldut --host --ff
```

### Server config (`soldut.cfg`)

Drop a `soldut.cfg` next to the binary to set match defaults
(loaded by the host on launch; CLI flags override the file):

```
port=23073
max_players=16
mode=ffa                       # ffa | tdm | ctf
score_limit=25
time_limit=600                 # seconds
friendly_fire=0
auto_start_seconds=60
map_rotation=foundry,slipstream,reactor
mode_rotation=ffa,ffa,tdm
```

Chassis names: `Trooper`, `Scout`, `Heavy`, `Sniper`, `Engineer`.
Primary weapons: `Pulse Rifle`, `Plasma SMG`, `Riot Cannon`,
`Rail Cannon`, `Auto-Cannon`, `Mass Driver`, `Plasma Cannon`, `Microgun`.
Secondaries: `Sidearm`, `Burst SMG`, `Frag Grenades`, `Micro-Rockets`,
`Combat Knife`, `Grappling Hook`. Armor: `None`, `Light`, `Heavy`,
`Reactive`. Jetpack: `Baseline`, `Standard`, `Burst`, `Glide`, `JumpJet`.
(Case-insensitive prefix match for weapons, exact for the rest.)

## Cross-compiling

`zig cc` is the cross compiler. With Zig on `PATH`:

```bash
./cross-windows.sh    # → build/windows/Soldut.exe
./cross-macos.sh      # → build/macos/Soldut.app   (requires macOS SDK; see script)
```

## Visual debugging — shot mode

For physics / motion / weapon questions, the binary takes a
`--shot path/to/script.txt` flag that drives a scripted scene through
the real renderer + sim and writes PNGs (and a paired `.log`) to
`build/shots/`. See `tests/shots/m3_*.shot` for in-match examples
(Mass Driver, Plasma SMG, Frag Grenade, Riot Cannon spread, Rail
Cannon charge, armor bar).

```bash
./soldut --shot tests/shots/m3_mass_driver.shot
make shot                                       # default script
make shot SCRIPT=tests/shots/m3_grenade.shot    # any other .shot
```

### Networked shot tests (M4)

Paired host + client shotmode runs talk over a real ENet loopback.
The driver script `tests/shots/net/run.sh` orchestrates both sides
and writes PNGs from each player's perspective to
`build/shots/net/{host,client}/`:

```bash
./tests/shots/net/run.sh 2p_basic       # sit in lobby/match/summary
./tests/shots/net/run.sh 2p_meet        # both walk to the same spot
./tests/shots/net/run.sh 2p_kill        # multi-round; roles reverse on each kill
./tests/shots/net/run.sh 2p_summary     # captures the "Next round in N s" sync
```

Add `network host PORT` / `network connect HOST:PORT` directives to
a `.shot` script to enable. `peer_spawn SLOT WX WY` lets the host
override authoritative spawn positions for kill-test layouts. See
`src/shotmode.h` for the full script grammar.

### Log-driven network smoke tests

`tests/net/run.sh` and `tests/net/run_3p.sh` spin up host + 1 or 2
clients, run a round end-to-end against real loopback, and assert
on per-side log-line milestones (handshake → mech_id resolve →
round end). No display required — they spawn raylib windows
briefly but always reap them via `trap cleanup EXIT`.

### Debugging with gdb

```bash
make debug                # → soldut-dbg (-O0 -g3 + ASan/UBSan)
make gdb                  # launch under gdb with project helpers
make gdb-host             # gdb + --host 23073
make gdb-client HOST=...  # gdb + --connect
make valgrind             # run headless_sim under memcheck
```

`tools/gdb/init.gdb` adds project helpers: `pelv N` (pelvis pos
for mech N), `mechs`, `lobby`, `match`, `net`, `bp_snap` (silent
breakpoint at `snapshot_apply`).

## Layout

```
soldut/
├── src/                 # game source (~33 modules / 68 .c+.h files post-M5 P08)
├── assets/
│   └── maps/            # generated by tools/cook_maps + editor shots (gitignored)
├── third_party/
│   ├── raylib/          # vendored, built into libraylib.a per platform
│   ├── enet/            # vendored, built into libenet.a
│   ├── stb_ds.h         # the only stb header we vendor directly
│   └── stb_sprintf.h
├── documents/           # design canon — read these before reading code
├── tools/
│   └── gdb/init.gdb     # project-specific gdb helpers
├── tests/
│   ├── shots/           # scripted scenes for the --shot harness
│   │   └── net/         # paired host+client networked shot tests
│   └── net/             # log-driven network smoke tests
├── Makefile             # native + debug + gdb + valgrind targets
├── build.sh             # one-shot orchestration
├── cross-windows.sh     # cross-compile to Windows via zig cc
├── cross-macos.sh       # cross-compile to macOS via zig cc
└── README.md
```

The design documents in [documents/](documents/) are the source of truth
for *intent*; the code is the source of truth for *behavior*. Read them in
order — they cross-reference each other.

## Status

### M0 — Foundation (done)

- [x] Project scaffolding, `Makefile`, `build.sh`
- [x] raylib + ENet + stb headers vendored, statically linked
- [x] Build matrix (`make`, `make windows`, `make macos`)
- [x] CI runs the build matrix on every push
- [x] Window, clear-color, FPS counter
- [x] Logger, arena allocator, pool allocator, math primitives

### M1 — One mech, no network (done)

- [x] Particle pool (SoA) + Verlet integrator
- [x] Constraint pool + 12-iter solver, constraints + map collisions interleaved per iter
- [x] Trooper chassis with the 16-particle skeleton + 21 distance constraints
- [x] Tile-grid map format + hard-coded tutorial level
- [x] Body-vs-map collision (per-particle vs tile rect, neighbour-aware exit)
- [x] Pose-driven animation (kinematic translate; layout-consistent targets): Stand, Run, Jet, Fall, Fire, Death
- [x] Post-physics anchor that holds standing pose without sag (see [TRADE_OFFS.md](TRADE_OFFS.md))
- [x] `Camera2D` smoothed follow + screen shake
- [x] Pulse Rifle hitscan, tracer, recoil impulse
- [x] Static target dummy that takes damage
- [x] Death: drop pose drive, apply killshot impulse, ragdoll
- [x] Limb dismemberment (left arm; constraint deletion → free ragdoll piece)
- [x] Blood particles + decal RT splat layer
- [x] Hit-pause + screen shake
- [x] HUD: health bar, jet fuel gauge, ammo, crosshair, kill feed
- [x] Headless physics tester (`make test-physics && ./build/headless_sim`)

### M2 — Networking foundation (done; two-laptop bake pending)

- [x] ENet wrapper (`src/net.{h,c}`), 4 channels (STATE / EVENT / CHAT / LOBBY)
- [x] Connection handshake (CONNECT_REQUEST / CHALLENGE / CHALLENGE_RESPONSE / ACCEPT)
- [x] LAN broadcast discovery (`DISCOVERY_QUERY` / `DISCOVERY_REPLY`)
- [x] Direct connect by IP+port (`--connect host[:port]`)
- [x] Client input upload (60 Hz) with sequence numbers
- [x] Server simulate loop (60 Hz) consuming inputs
- [x] Server snapshot broadcast (30 Hz) — full snapshots, no delta yet
- [x] Client-side prediction + reconciliation for the local mech
- [x] Lag compensation for hitscan (12-tick bone-history ring per mech, capped at 200 ms)

### M3 — Combat depth (done)

- [x] All 5 chassis with parameterized stats (Trooper / Scout / Heavy / Sniper / Engineer)
- [x] All 8 primaries (Pulse Rifle, Plasma SMG, Riot Cannon, Rail Cannon, Auto-Cannon, Mass Driver, Plasma Cannon, Microgun)
- [x] All 6 secondaries (Sidearm, Burst SMG, Frag Grenades, Micro-Rockets, Combat Knife; Grappling Hook landed at M5 P06)
- [x] Projectile pool (`src/projectile.{h,c}`, capacity 512) with gravity, drag, swept tile + bone collision
- [x] Explosions: damage falloff (`1 - (d/r)²`), line-of-sight halving, impulse to live mechs and ragdolls
- [x] Hit-location damage multipliers (head ×1.6, arms/legs ×0.7, hands/feet ×0.5)
- [x] Per-limb HP for all 5 limbs; dismemberment via constraint deactivation; head detach is also lethal
- [x] All 4 armor variants (None / Light / Heavy / Reactive); Reactive eats one explosion fully
- [x] All 4 jetpack variants (Standard / Burst / Glide Wing / Jump Jet)
- [x] Recoil (hand impulse), bink (incoming-fire flinch on nearby targets), self-bink (rapid-fire jitter)
- [x] Friendly-fire toggle (`--ff` server flag), default off
- [x] Kill feed UI with HEADSHOT / GIB / OVERKILL / RAGDOLL / SUICIDE flags
- [x] Loadout via CLI flags (`--chassis`, `--primary`, `--secondary`, `--armor`, `--jetpack`)
- [x] Snapshot wire format widened (+5 bytes/mech) to carry chassis + armor + jetpack + secondary; protocol bumped to `S0LE`

### M4 — Lobby & matches (done)

- [x] Game flow: title → server browser → connect → lobby → countdown → match → summary → next round
- [x] Server browser (LAN broadcast discovery, refresh + direct-connect)
- [x] Lobby UI: player list with team chips, chat with rate limiting, mech selector + 5-slot loadout picker, ready button
- [x] FFA mode (per-player kill cap + time limit)
- [x] TDM mode (Red/Blue team selector, friendly-fire toggle)
- [x] CTF wired through the protocol (plays as TDM at M4; flag mechanics land at M5)
- [x] Round summary: per-player scoreboard with kills/deaths/score, MVP highlight, "Next round in N s" countdown
- [x] Map cycle: 3 code-built maps (Foundry / Slipstream / Reactor) rotated via config
- [x] Server config file (`soldut.cfg`): port, max_players, mode, score_limit, time_limit, friendly_fire, auto_start_seconds, map/mode rotations
- [x] Kick / ban backend (host-only `LOBBY_KICK` / `LOBBY_BAN` messages; host-controls UI buttons land at M5)
- [x] Per-frame UI scale factor (1× at 720p → 3× at 4K, snaps in 0.25 steps)
- [x] MSAA 4× + bilinear-filtered default font for crisp UI at any resolution
- [x] Snapshot velocity sync + 35 % remote-mech smoothing (the cheap version of "render 100 ms in the past")
- [x] Blood + decal sync to the client via health-drop / death-transition detection in `snapshot_apply`
- [x] Networked shot tests (`tests/shots/net/run.sh`) — paired host + client runs with PNG screenshots from each side
- [x] Log-driven network smoke tests (`tests/net/run.sh`, `tests/net/run_3p.sh`) — 13 + 10 assertions covering the full round flow
- [x] gdb workflow: `make debug`/`gdb`/`gdb-host`/`gdb-client`/`valgrind` + project-specific helpers in `tools/gdb/init.gdb`
- [x] Protocol bumped `S0LE` → `S0LF` for the M4 lobby flow

### M5 — Maps & content (in progress)

Sequenced through 18 implementation prompts under
[documents/m5/prompts/](documents/m5/prompts/). Read
[documents/m5/00-overview.md](documents/m5/00-overview.md) for the
strategic view.

- [x] **P01** — `.lvl` binary format + loader/saver + CRC32 + tests (`src/level_io.{c,h}`; `make test-level-io` 25/25 passing). 64-byte header + WAD-style lump directory + 9 lumps; `_Static_assert` size locks on every record type.
- [x] **P02** — Polygon collision + slope physics + slope-aware post-physics anchor. Per-particle Q1.7 contact normals + `PARTICLE_FLAG_CEILING`; polygon broadphase grid built at level load; tile + polygon collision interleaved per relaxation iter; slope-tangent run velocity + slope-aware friction; angled-ceiling jet redirection; WIND/ZERO_G ambient zones; DEADLY/ACID environmental damage.
- [x] **P03** — Render-side accumulator + interp alpha + reconcile smoothing + remote-mech interp. Per-particle `render_prev_*` snapshot at top of `simulate_step`; renderer lerps by `alpha = accum/TICK_DT`; reconcile `visual_offset` decays over ~6 frames; per-mech `remote_snap_ring[8]` interp at `now − 100 ms`; `SNAP_STATE_IS_DUMMY` bit; `NET_MSG_HIT_EVENT` + `NET_MSG_FIRE_EVENT` for blood/spark/tracer/projectile sync. Protocol bumped `S0LF` → `S0LG`.
- [x] **P04** — Standalone level editor (`tools/editor/`) + game-side `--test-play <path>` flag. `make editor` → `build/soldut_editor`. Tile paint, polygon draw with ear-clipping triangulation (Eberly), spawn / pickup / ambient / decoration placement, slope + alcove presets, undo/redo (64-stroke ring + tile-grid snapshot for big ops), save-time validation (alcove sizing per the maps spec), F5 forks the game with the saved `.lvl`. raygui (header-only, vendored at `third_party/raygui/`) for the UI; in-app textbox modal in lieu of a native file picker. Editor links a subset of `src/` (level_io / arena / log / hash / ds) — no mech / physics / net.
- [x] **P05** — Pickup runtime + Engineer deployable repair pack + Burst SMG cadence + practice dummy + powerups (`make test-pickups` 43/43). New module `src/pickup.{c,h}`; `PickupPool` (cap 64) on `World`. Per-kind apply rules (HEALTH / AMMO / ARMOR / WEAPON / POWERUP / JET_FUEL / REPAIR_PACK / PRACTICE_DUMMY) with full-state reject guards. Powerup state on `Mech` (`powerup_berserk/invis/godmode_remaining`); berserk doubles outgoing damage and godmode zeroes incoming damage in `mech_apply_damage`; client mirrors via new `SNAP_STATE_BERSERK/INVIS/GODMODE` bits (no protocol bump — fits in P03's u16). Engineer's `BTN_USE` now spawns a transient repair pack at the engineer's feet (10 s lifetime, 30 s CD). Burst SMG queues round 1 + 2 trailing rounds at 70 ms cadence in `mech_step_drive`. Per-mech 4-anchor (chest/pelvis/feet) touch detection so pickups grab regardless of body height. New 20-byte `NET_MSG_PICKUP_STATE` carries full spawner data so transients (engineer pack) replicate to clients; `pickupfeed[64]` queue drained by `broadcast_new_pickups` in main.c. Foundry default map ships 4 example pickups (HEALTH / INVIS / AMMO / BERSERK / ARMOR). Top-center HUD pill displays each active powerup + remaining seconds. Editor's `palette.c` migrated to canonical `PickupKind` enum (was off-by-one against runtime, caused F5 BAD_SECTION errors); editor `Makefile` now tracks header deps (`-MMD -MP`).
- [x] **P06** — Grappling hook (full implementation, replacing M3's `WFIRE_GRAPPLE` stub). New per-`Mech` `Grapple` struct + `PROJ_GRAPPLE_HEAD` projectile + `CSTR_FIXED_ANCHOR` constraint kind (Constraint grows by `Vec2 fixed_pos`); `mech_try_fire`'s WFIRE_GRAPPLE branch spawns a 1200 px/s head from R_HAND; on hit `projectile_step` sets state to ATTACHED + calls `mech_grapple_attach` which appends a constraint to the global pool (CSTR_FIXED_ANCHOR for tile, CSTR_DISTANCE_LIMIT min=0/max=L for bone — both **one-sided** so the rope is slack when shorter than rest and taut when stretched, the "Tarzan swing" feel). Initial rope length is the hit-time pelvis-to-anchor distance, **clamped to [80, 300] px** so cross-screen fires still give a tight swingable rope. `mech_step_drive`'s server-side ATTACHED block: BTN_JET (W) **retracts** the rope at 800 px/s clamped at 100 px (clears the body height so a ceiling anchor doesn't crush the head into a tile); BTN_USE releases; BTN_FIRE while ATTACHED chain-releases + fires a new head. New `SNAP_STATE_GRAPPLING` bit (1u<<12) gates an optional 8-byte trailing suffix on `EntitySnapshot` (state / anchor_mech / anchor_part / anchor_x_q / anchor_y_q) so idle bandwidth stays flat. New `draw_grapple_rope` in `render.c` draws a 1.5 px gold line hand → live head (FLYING) or hand → anchor (ATTACHED). Editor side: a Loadout modal (`L` key in editor or `[L] Loadout` button) lets you set chassis / primary / secondary / armor / jetpack via dropdowns; F5 forwards the picks to the spawned game (matching `--test-*` CLI args also accepted). Game shotmode gains a `load_lvl <path>` script directive so a custom `.lvl` (built by an editor shot) can drive a game shot — used by `tests/shots/m5_grapple_ceiling.shot` to verify the swing physics on a ceiling map (`make test-grapple-ceiling`). Three new game shot tests (`m5_grapple.shot`, `m5_grapple_swing.shot`, `m5_grapple_ceiling.shot`) and three new editor shot tests (`loadout.shot`, `loadout_dropdowns.shot`, `grapple_test_map.shot`) ride alongside.
- [x] **P07** — CTF mode (`src/ctf.{c,h}`, `make test-ctf` 52/52). Server-authoritative `Flag flags[2]` on `World`, populated by `ctf_init_round` from `level.flags` LvlFlag records (flags[0]=RED, flags[1]=BLUE convention). `ctf_step` per tick: 36 px touch detection, 30 s auto-return, **both-flags-home** capture rule (carrier touching own HOME flag while carrying the enemy flag → +5 team / +1 slot, captured flag returns home). `ctf_drop_on_death` from `mech_kill` drops at pelvis pos with `FLAG_AUTO_RETURN_TICKS` pending. Carrier penalties: `apply_jet_force` halves thrust, `mech_try_fire` rejects when `active_slot == 1` (entire secondary disabled). New wire message `NET_MSG_FLAG_STATE = 16` (variable: 1 byte tag + 1 byte flag_count + 12 bytes per flag — team / status / carrier_mech / pos_q / return_in_ticks; max 26 bytes). `INITIAL_STATE` appends optional flag-state suffix for joining clients. CTF score limit defaults to `FLAG_CAPTURE_DEFAULT = 5` when config has the FFA-default `>=25`. Mode-mask validation in `start_round` demotes CTF to TDM if the rotation lands on an incompatible map. Render: `draw_flags` after mech body — vertical staff + triangular pennant team-colored, sin-driven wobble while CARRIED, outline halo for DROPPED. HUD: `draw_flag_pips` + `draw_flag_compass` (off-screen flag → triangular arrow at the nearest screen edge). New code-built map `MAP_CROSSFIRE` (140×42 symmetric arena, 8 spawns / 5 example pickups, `mode_mask = FFA|TDM|CTF`). TDM/CTF team auto-balance in `start_round` distributes slots RED / BLUE deterministically. Editor `TOOL_FLAG` (with `F` shortcut) drops Red+Blue flag pairs and auto-toggles `META.mode_mask` CTF bit; F5 `--test-play` auto-detects CTF maps and forces `mode = CTF` + `score_limit = 5`. Snapshot pos quant 8× → 4× post-fix (range now ±8190 px, fits Crossfire's 4480 px width).
- [x] **P08** — Map sharing across the network (`src/map_cache.{c,h}` + `src/map_download.{c,h}`; `make test-map-chunks` 21/21 + `tests/net/run_map_share.sh` 15/15). 36-byte `MapDescriptor` (crc + size + short_name) appended to every `NET_MSG_INITIAL_STATE`; four new tags on `NET_CH_LOBBY` — `MAP_REQUEST=40` / `MAP_CHUNK=41` / `MAP_READY=42` / `MAP_DESCRIPTOR=43`. Chunks are 1180-byte payloads behind a 16-byte header (fits ENet's 1200 MTU). Content-addressed cache at platform-specific paths: Linux `$XDG_DATA_HOME/soldut/maps/`, macOS `~/Library/Application Support/Soldut/maps/`, Windows `%APPDATA%\Soldut\maps\`. Cache writes are atomic (`<crc>.lvl.tmp` + rename) and capped at 64 MB (LRU eviction by mtime). Client resolve order: `assets/maps/<short>.lvl` with matching CRC → `<cache>/<crc>.lvl` → download. ROUND_START builds via `map_build_for_descriptor` (new helper in maps.c) so cached maps load without polluting `assets/`. Host's auto-start countdown holds at ≥1.5 s while any peer's `map_ready_crc` doesn't match the current map crc (slow downloaders extend the round; the gate keeps spawn from happening before they have geometry). Lobby UI shows a `DOWNLOADING MAP NN%` strip while `g->map_download.active`. Mid-lobby host map changes (mode/map cycle) re-broadcast `NET_MSG_MAP_DESCRIPTOR` so clients re-resolve. Trust model: 2 MB `MapDescriptor.size_bytes` cap, OOB chunk rejection, CRC verify on reassembled buffer (mismatch → MAP_READY status=CRC_MISMATCH + log + don't write cache), 30 s stall watchdog disconnects. `MapDownload` lives on the permanent arena (2 MB buffer + 1792-bit chunk-received bitmap, reused across rounds). Protocol id stays `S0LG` (additive). Version bumped to `0.0.7-m5p08`.
- [ ] **P09** — `BTN_FIRE_SECONDARY` (RMB) + keybinds + UI controls panel
- [ ] **P10** — Mech sprite atlas runtime + per-chassis bone distinctness
- [ ] **P11** — Per-weapon visible art + two-handed foregrip
- [ ] **P12** — Damage feedback (hit-flash, decals, stump caps, smoke)
- [ ] **P13** — Parallax + HUD final art + TTF font + halftone post + decal chunking
- [ ] **P14** — Audio module (`src/audio.{c,h}`)
- [ ] **P15–P16** — ComfyUI asset generation (chassis, weapons, HUD icons)
- [ ] **P17–P18** — Author 8 `.lvl` maps + bake-test harness

## License

The game is MIT-licensed (see `LICENSE` once added). Vendored dependencies
keep their own licenses:

- raylib — zlib/libpng
- ENet — MIT-style
- stb_ds.h, stb_sprintf.h — public domain (Sean Barrett)
