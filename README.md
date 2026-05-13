# Soldut

A 2D side-scrolling multiplayer mech shooter in C, in the lineage of Soldat.

**M6 — Stability & ship prep** in flight (2026-05-13). P01 (IK + pose
sync), P02 (jetpack propulsion FX), P03 (capped internal render
target / "4K" FPS fix), and P04 (map balance + bot AI hardening)
shipped; crash logging, code signing, and the 32-player stress test
remain. P04 added a layered classical bot AI module
(`src/bot.{c,h}`) with full lobby integration + tier chip UI, then
wired a 320-cell bake-test sweep (8 maps × 5 chassis × 8 primaries)
to drive iteration on 4 of 8 maps + the bot AI until every ship map
produces real combat at Champion tier. See
[`documents/m6/04-map-balance.md`](documents/m6/04-map-balance.md)
for the per-map loadout findings.

**Post-P04 polish (2026-05-13)**: bot management rewritten to be
per-bot — host setup screen no longer carries bot controls; the
lobby's second row holds a `Tier:` cycle + `[+ Add Bot]` button so
the host picks the pending tier and clicks Add once per bot, with
each bot keeping its own tier. Per-bot removal via the `Remove`
button on the bot's row in the player list. New
`NET_MSG_LOBBY_ADD_BOT` wire (id 35) carries the host-UI client's
request to the in-process server. Bots now also auto-cast random
votes on the post-round map vote so matches with humans + bots
fast-forward as soon as the humans have picked. Bot AI's jet
behavior is now fuel-aware (10 %–40 % lockout hysteresis) so bots
no longer mash JET against an empty tank — they wait for the gauge
to refill and re-engage. Cycle buttons gained directional hit
regions: left quarter cycles backward regardless of mouse button,
right quarter cycles forward regardless, middle keeps LMB-fwd /
RMB-back. Host-setup Map button uses the cycle button now.

**M5 — Maps & content** complete (2026-05-12). P01–P19 in, plus off-roadmap wan-fixes 1–16, plus seven post-P19 follow-up rounds that retired bugs surfaced by paired-window LAN playtests (audio loudnorm wrecking short transients, jetpack/servo masking footsteps, the wan-fixes-3 inv_mass race that broke joining-client physics, the slope-tangent stretch that inflated mechs uphill, the pose-drive feedback loop that stretched remote-mech bones).
M4 shipped the lobby & matches layer: full game flow
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
**P01** — `.lvl` binary format + loader/saver; **P02** — polygon
collision + slope physics + slope-aware post-physics anchor;
**P03** — render-side accumulator + interp alpha + reconcile
smoothing + two-snapshot remote-mech interp + hit/fire event sync;
**P04** — standalone level editor (`tools/editor/`, raygui-based)
+ game-side `--test-play <path>` flag; **P05** — pickup runtime,
Engineer's deployable repair pack, Burst SMG 70 ms cadence,
practice-dummy spawner kind, active-powerup HUD pill; **P06** —
Grappling Hook (one-sided "Tarzan" rope, hold W to retract, BTN_FIRE
chain-refire while attached, BTN_USE to release); **P07** — CTF
mode (`src/ctf.{c,h}`, both-flags-home capture rule, 36 px touch /
30 s auto-return, half-jet + secondary-disabled carrier penalties,
drop-on-death, team-colored staff-and-pennant render with
off-screen HUD compass); **P08** — Map sharing across the network
(content-addressed cache, lobby UI download progress, host gate on
per-peer MAP_READY); **P08b** — runtime `MapRegistry` that surfaces
user-authored `assets/maps/*.lvl` files in the lobby cycle without
overwriting builtin slots; **P09** — `BTN_FIRE_SECONDARY` (RMB)
one-shot of inactive slot, host kick/ban modal + `bans.txt`
persistence, three-card map vote picker on summary, title-screen
Controls modal, plus the **multi-round match flow**: configurable
`rounds_per_match`, "1 player remaining → 3 s warning → end"
auto-end rule, all-voted summary fast-forward, seamless inter-round
COUNTDOWN (no lobby between rounds), and back-to-lobby on match-over;
**P10** — mech sprite atlas runtime (`src/mech_sprites.{c,h}`,
sprite-or-capsule dispatch in `draw_mech`, 17-entry render-parts
z-order with L↔R swap when facing left) + per-chassis bone-length
distinctness pass (Heavy bigger torso, Scout shrunk, Sniper
anisotropic, Engineer compact); **P11** — per-weapon visible art
runtime (`src/weapon_sprites.{c,h}`, 14-entry sprite-def table with
grip / foregrip / muzzle pivots, line fallback when atlas absent,
RMB hand-flicker shows the inactive slot for 3 ticks, 3-tick muzzle
flash); **P12** — damage feedback layers (whole-mech hit-flash
white-additive blend on every damage event, per-limb persistent
damage decals stored in sprite-local i8 coords, 64-particle blood
spray + new `FX_STUMP` pinned emitter on dismemberment that tracks
the parent particle for ~1.5 s, smoke from limbs at <30% HP with
squared-deficit RNG roll, host/client sync via the existing
`NET_MSG_HIT_EVENT`); **P13** — rendering kit (TTF fonts —
Atkinson Hyperlegible / VG5000 / Steps Mono — vendored under
`assets/fonts/` and loaded via `LoadFontEx`; halftone post-process
fragment shader at `assets/shaders/halftone_post.fs.glsl` wired
through a backbuffer-sized `RenderTexture2D`; new
`src/map_kit.{c,h}` for per-map parallax + tile-atlas bundle from
`assets/maps/<short>/`; 3-layer parallax with screen-space tile
wraparound; tile sprites with 2-tone fallback; free polygon
rendering refactored out of the P02 stopgap with M5 spec colors
+ BACKGROUND-poly pass after mechs at α=0.6; decoration sprites
with 4-layer dispatch + ADDITIVE/FLIPPED_X flags + hash-based
sub-rect placeholder; HUD final art with atlas-aware bars + tick
marks + bink-tinted crosshair + weapon-icon kill-feed entries;
`src/decal.c` chunked into 1024×1024 lazy-allocated tiles when
level >4096 px; new `src/hotreload.{c,h}` DEV_BUILD-gated mtime
watcher polling every 250 ms with reload callbacks for chassis /
weapons / decorations / HUD / shader); **P14** — audio module
(`src/audio.{c,h}` with 47-entry SFX manifest + alias pool, 5
software buses with default mix master/sfx/music/ambient/ui =
1.00/1.00/0.30/0.45/0.70, listener-relative `audio_play_at`
(200..1500 px attenuation, ±800 px pan) + `audio_play_global`,
`audio_request_duck` stacks via min on music+ambient for big
detonations (≥100 dmg AOE → 0.5× for 300 ms), per-frame
`audio_step` runs duck recovery + `UpdateMusicStream` + ambient
retrigger + servo modulation; servo loop modulated by local-mech
velocity at `clamp(vel/280) × 0.7` with 0.15 lerp; `LoadMusicStream`
looping per-map via `apply_audio_for_map` resolving the level's
META `music_str_idx` + `ambient_loop_str_idx` from the STRT lump
(hard cut between map tracks, no crossfade); ~30 call sites wired
across `weapons.c` (fire SFX in all four fire paths + HIT_FLESH /
HIT_CONCRETE), `projectile.c` (size-bucketed explosion SFX +
ducking + GRAPPLE_HIT), `mech.c` (footsteps via gait wrap, jet
pulse rate-limited every 4 ticks, grapple FIRE/RELEASE,
KILL_FANFARE for local killer + DEATH_GRUNT at victim),
`pickup.c` (per-kind grab + RESPAWN cue for high-tier),
`ctf.c` (host-side flag SFX) + `client_handle_flag_state` mirror
(so pure clients hear flag events too), `ui.c` (UI_CLICK +
UI_TOGGLE), `client_handle_fire_event` (mirrored fire SFX gated
on `!predict_drew`), `client_handle_hit_event` (HIT_FLESH); hot
reload registers the full SFX manifest + servo path via
`audio_register_hotreload` (DEV_BUILD-gated, fits in P13's
64-entry watcher cap)). Asset generation (chassis + weapons + HUD + Foundry
parallax) shipped at P15–P16; **P17** ships the first batch of 4
authored `.lvl` maps (Foundry / Slipstream / Reactor / Concourse) via
a new `tools/cook_maps/` exporter (`make cook-maps`); **P18** adds
4 more maps (Catwalk / Aurora / Crossfire / Citadel) plus a headless
bake-test harness; **P19** fills the P14 audio runtime with 62 CC0
assets (47 SFX + 1 servo + 7 music + 7 ambient) from Kenney + opengameart
via `tools/audio_inventory/source_map.sh`, with peak normalization
for short transients (LUFS loudnorm pulled brief impacts down to
-25 dB peak — bad). F2 toggles a global mute for paired-window
LAN tests. Multiple **post-P19 fixes** followed: pause/resume music
on mute (raylib's SetMusicVolume(0) is unreliable), `apply_audio_for_level`
moved to the audio module so the client's `client_handle_round_start`
calls it (was host-only), remote-mech `grounded` flag preserved from
snapshot (was clobbered by `any_foot_grounded` returning false on
kinematic bodies), wan-fixes-3 inv_mass loop set both sides every
tick (was zero-only — joining client's mech got permanently zeroed
during the local_mech_id resolution race), slope-tangent Y-velocity
applied uniformly to every particle (was lower-chain only — inflated
the body on uphill runs), `apply_pose_to_particles` skipped for
kinematic remote mechs (was driving a build_pose feedback loop that
inflated bones).
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

A 1920×1080 window opens at the **title screen** with five entries:
Single Player, Host Server, Browse Servers (LAN), Direct Connect,
Quit. Pick one and the lobby flow takes over — pick chassis +
loadout + team, hit Ready, the round starts on a 3-second countdown
once everyone is ready (or on a 60s auto-start once half the slots
are filled). Player picks persist to `soldut-prefs.cfg` next to the
binary so loadout + name + last-connected address survive between
launches.

The host UI spawns a `--dedicated PORT` child of the same binary
and connects to it as a regular client — so both peers experience
identical latency and rendering. See [wan-fixes log](#wan-fixes-log)
for the iteration history.

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
| `Left Mouse`   | Fire active slot (re-press while grappled chains a new grapple) |
| `Right Mouse`  | Fire **inactive** slot (one-shot RMB) — primary stays active |

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
rounds_per_match=3             # also accepts match_rounds=
map_rotation=foundry,slipstream,reactor
mode_rotation=ffa,ffa,tdm
```

A "match" is `rounds_per_match` rounds. Players ready up once at the
start of a match; rounds within a match transition seamlessly (vote
picker → brief countdown → respawn) without showing the lobby. After
the final round, the lobby reopens for fresh ready-up.

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
├── src/                 # game source (~42 modules / 78 .c+.h files post-M5 P14)
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
- [x] **P08b** — Runtime `MapRegistry` (32-slot cap) replaces the static 4-entry `MapId` enum. `game_init` scans `assets/maps/*.lvl` cheaply (header + META lump only, no `level_load`) so user-authored maps surface in the host's lobby cycle without overwriting builtin slots. The four named constants (`MAP_FOUNDRY`..`MAP_CROSSFIRE`) stay as load-bearing reserved indices for `build_fallback`. Builtin overrides are by short_name match — saving `assets/maps/foundry.lvl` from the editor stamps the builtin's slot with disk CRC + size + on-file mode_mask. Custom-map IDs (≥`MAP_BUILTIN_COUNT`) get a hard-fallback path in `map_build` (LOG_E + Foundry's code-built) instead of falling through to `build_fallback`'s switch default. New regression `tests/map_registry_test.c` and end-to-end `tests/shots/net/run_meet_named.sh` (18 assertions) prove the editor → host → client flow on a non-reserved name.
- [x] **P09** — `BTN_FIRE_SECONDARY` (RMB, bit 12 in the input bitmask) one-shots the inactive slot via new `fire_other_slot_one_shot` in `src/mech.c` (swap-dispatches through the existing `weapons_fire_*` helpers — HITSCAN / PROJECTILE / SPREAD / THROW / BURST / MELEE / GRAPPLE; charge weapons rejected; flag carrier secondary still gated; shared `fire_cooldown` prevents LMB+RMB DPS doubling). New `lobby_load_bans` / `lobby_save_bans` round-trip a flat `<addr_hex> <name>` `bans.txt` next to the binary; `bootstrap_host` loads on launch and `lobby_ban_addr` auto-saves on update. Lobby UI: row-hover `[Kick] [Ban]` buttons in `player_row` for non-host slots when `net.role == NET_ROLE_SERVER`, click stages a Cancel/Confirm modal in `lobby_screen_run`. Three-card map vote picker on the summary screen: `host_start_map_vote` Fisher-Yates picks 3 distinct mode-compatible maps from `g_map_registry`, broadcasts `NET_MSG_LOBBY_VOTE_STATE`, winner overrides `match.map_id` on `begin_next_lobby`. Title-screen Controls modal sourced from `src/platform.c` so it shows the actual game keys including the new RMB binding. Multi-round match flow: `rounds_per_match` from cfg or `--rounds`; "1 player remaining → 3 s warning → end" auto-end rule; all-voted summary fast-forward; seamless inter-round COUNTDOWN (no lobby between rounds); back-to-lobby on match-over. Resolves three M4-era trade-offs: "Map vote picker UI is partial", "Kick / ban UI not exposed", "`bans.txt` not persisted".
- [x] **P10** — Mech sprite atlas runtime + per-chassis bone-length distinctness pass. Aggressive bone-length tweaks in `g_chassis[]` (`src/mech.c`): Heavy bumped torso 34→38 + neck 14→16, Scout shrunk -16% all over, Sniper anisotropic with forearm 17→19 + shin 18→21, Engineer compact with forearm 16→14 + thigh 18→16. Per-chassis posture quirks in `build_pose`: Scout forward chest lean, Heavy chest strength 0.7→0.85, Sniper head down+forward 3 px, Engineer skips right-arm aim drive when `active_slot == 1`. New module `src/mech_sprites.{c,h}` with `MechSpriteSet g_chassis_sprites[CHASSIS_COUNT]` (22-entry placeholder sub-rect/pivot/draw-size table per chassis). Sprite-or-capsule dispatch in `draw_mech` based on `g_chassis_sprites[chassis].atlas.id != 0` — new `draw_mech_sprites` walks 17-entry `g_render_parts[]` z-order table with L↔R swap when `facing_left`; `draw_mech_capsules` (M4 path) preserved as the no-asset fallback. Atlas loaded via `mech_sprites_load_all` after `platform_init` in main.c + shotmode.c; missing files leave `atlas.id == 0` so a fresh checkout still renders capsules. New shotmode directive `extra_chassis <name> <x> <y>` (cap 6) spawns side-by-side dummies; new shot test `tests/shots/m5_chassis_distinctness.shot` puts all 5 chassis in one frame for visual verification.
- [x] **P11** — Per-weapon visible art runtime + two-handed foregrip pose attempt (reverted post-ship). New module `src/weapon_sprites.{c,h}` with 14-entry `g_weapon_sprites[]` table (sub-rect + grip / foregrip / muzzle pivots in source-relative px + draw_w/h). Shared `Texture2D g_weapons_atlas` from `assets/sprites/weapons.png`; missing file falls back to a per-weapon-sized line render in `draw_held_weapon` (`draw_w * 0.7` per weapon → Mass Driver still reads visibly longer than a Sidearm). New helpers `weapon_muzzle_world(...)` + `weapon_foregrip_world(...)` apply the same world-space transform (sprite-local +X along aim, +Y perpendicular) so visible muzzle and physics muzzle line up exactly. New `draw_held_weapon` in `src/render.c` (called from `renderer_draw_frame` after `draw_mech`, before `draw_grapple_rope`) picks an effective weapon via new `Mech.last_fired_slot` / `last_fired_tick` fields (RMB-fire-secondary triggers a 3-tick hand-flicker swap to the inactive slot's sprite, then reverts) + visible muzzle flash (additive yellow-orange disc, alpha-tapered over 3 ticks). Migrated four fire paths (`weapons_fire_hitscan`, `_lag_comp`, `weapons_predict_local_fire`, `weapons_spawn_projectiles`) plus both grapple branches in `mech.c` from `Weapon.muzzle_offset` (single float, kept as fallback) to `weapon_muzzle_world(...)`. Two-handed foregrip pose was attempted in three variants (strength-0.6 L_HAND yank, clamped, snap-pose-IK strength-1.0) and reverted post-ship — all three drifted the body in the aim direction during steady-hold because the L_ARM rest-state decouples from L_SHOULDER during the constraint-solve iterations as PELVIS shifts under R_ARM aim drive. Real fix is a 2-bone IK constraint inside the solver loop; deferred to M6. `WeaponSpriteDef.pivot_foregrip` stays on the table for the future IK consumer.
- [x] **P12** — Damage feedback layers (hit-flash + persistent decals + stump caps + smoke + heavy spray on dismember + host/client sync). `Mech.hit_flash_timer` set to 0.10 s on every successful damage event in `mech_apply_damage`, decayed each tick in `simulate_step`, modulated through new `apply_hit_flash` in `render.c` so the body tint blends toward white in BOTH the sprite path and the capsule fallback. Persistent damage decals on `Mech.damage_decals[MECH_LIMB_DECAL_COUNT]` — one ring per visible part (16-deep, oldest-overwrite via `count % 16`); each entry stores `(local_x, local_y, kind)` where local coords are sprite-midpoint-relative i8 (kind = `DAMAGE_DECAL_DENT` for <30 dmg, `_SCORCH` for ≥30, `_GOUGE` for ≥80). New `mech_part_to_sprite_id` + `mech_sprite_part_endpoints` helpers in `mech_sprites.{c,h}` map hit `PART_*` to its `MechSpriteId` and back to the bone endpoints so the world↔local transform stays consistent between record-time (`mech_apply_damage`) and render-time (`draw_damage_decals`). New FX kind `FX_STUMP` in `world.h` with `pin_mech_id` + `pin_limb` fields on `FxParticle`; `fx_spawn_stump_emitter` allocates a no-render emitter pinned to the limb's parent particle (NECK / SHOULDER / HIP), and `fx_update`'s new `FX_STUMP` branch looks up the parent every tick + spawns 1–2 downward-biased blood drops (skips gravity integrate + wall-collide; self-deactivates on invalid pin). `mech_dismember` now fires a 64-particle radial blood spray + arms the 1.5 s pinned emitter. `fx_spawn_smoke` mirrors `fx_spawn_blood` with darker render and ~1.0 s life; per-limb threshold check in `simulate_step` runs every 8th tick over alive mechs, computes `intensity = ((0.30 - frac) / 0.30)^2` per damaged limb (frac < 0.30 fires) and rolls `pcg32_float01 < intensity` to spawn at the limb's elbow / knee / head particle. New stump-cap render path in `draw_mech_sprites` walks `dismember_mask` and draws the chassis atlas's `MSP_STUMP_*` sub-rect at the parent particle (skipped silently when `set->atlas.id == 0` — capsule fallback intentionally has no stump-cap art per spec). Hit-flash / decals / spray / emitter / smoke all work in BOTH render paths so a fresh checkout (no chassis atlases) gets the same feedback. Client-side `NET_MSG_HIT_EVENT` handler now also kicks `victim->hit_flash_timer = 0.10f` and decrements the corresponding limb HP by the broadcast `damage` byte; snapshot apply now routes newly-set `dismember_mask` bits through `mech_dismember(w, mid, bit)` so the client also runs constraint deactivation + 64-particle spray + emitter pin when an arm/leg/head detaches. New shot tests `tests/shots/m5_dismember.shot` + `tests/shots/net/2p_dismember.{host,client}.shot` (both views show identical hit-flash + decals + dismember beat with matching world coords within 0.5 px).
- [x] **P13** — Rendering kit (TTF fonts + halftone post + per-map kit + parallax + tile sprites + free polygons + decoration sprites + HUD final art + decal chunking + hot reload). Three OFL faces shipped under `assets/fonts/` — Atkinson-Hyperlegible-Regular.ttf (~54 KB, body) + VG5000-Regular.otf (~75 KB, Velvetyne, display) + Steps-Mono-Thin.otf (~22 KB, raphaelbastide, mono). New extern globals `g_ui_font_body / _display / _mono` + `UIFontKind` enum + `ui_font_for(kind)` helper in `src/platform.{c,h}` with graceful `GetFontDefault()` fallback when a face fails to load. `src/ui.c` and `src/hud.c` migrated from `GetFontDefault()` to `ui_font_for`. Halftone post-process: new `assets/shaders/halftone_post.fs.glsl` (GLSL 330, Bayer 8x8 ordered-dither + 60% darkening for pixels under threshold; ~50 LOC). Wired via a new backbuffer-sized `RenderTexture2D g_post_target` + `Shader g_halftone_post` in `src/render.c` — world draws to RT, backbuffer blit applies the shader, HUD draws on top with no shader. Per-frame uniforms `resolution` + `halftone_density` (default 0.30). RT recreated on backbuffer-size change; missing shader file → graceful direct-to-backbuffer fallback. New module `src/map_kit.{c,h}` owns the per-map art bundle (`g_map_kit` global with parallax_far/mid/near + tiles `Texture2D` slots loaded from `assets/maps/<short>/`, hooked from `map_build` / `map_build_from_path` after `level_load` succeeds, no-asset fallbacks for every slot). New render-side helpers in `src/render.c`: `draw_parallax_far_mid` (screen-space, before BeginMode2D, ratios 0.10/0.40), `draw_parallax_near` (screen-space, after BeginMode2D, ratio 0.95), and `draw_level_tiles` (when atlas is loaded, walks `LvlTile.id` against an 8x8 grid of 32x32 sub-rects; falls back to M4 2-tone checkerboard otherwise). Free polygons extracted from the P02 stopgap into `draw_polys` (M5 spec colors per kind: SOLID 32,38,46 / ICE 180,220,240 / DEADLY 80,200,80 / ONE_WAY 80,80,100) + new `draw_polys_background` (BACKGROUND polys after mechs at α=0.6 as foreground silhouettes). Decoration sprites via lazy-loaded `g_decorations_atlas` (4-layer dispatch: 0+1 before tiles, 2 after tiles before mechs, 3 after mechs/projectiles/fx; ADDITIVE flag wraps in `BeginBlendMode(BLEND_ADDITIVE)`; FLIPPED_X flips source rect; sub-rect picked via `sprite_str_idx` hashed into 16x16 grid of 64x64 cells until P15/P16 ships a real manifest; missing-atlas fallback paints layer-tinted placeholder rectangles so designers see deco placement during test-play). HUD final art: lazy-loaded `g_hud_atlas` from `assets/ui/hud.png` (256×256). New `draw_bar_v2` (1px outline + dark bg + fg fill + tick marks every 10% — works in both atlas + no-atlas paths since the layout is identical), `hud_draw_weapon_icon` + `hud_draw_killflag_icon` (atlas sub-rect or per-id color-swatch / short-text fallback), bink-tinted crosshair (red >0.5, pale-cyan otherwise; atlas-aware sprite path with M4 line-cross fallback). Kill-feed entries replaced text-only weapon name with weapon-icon + name + flag-icon variants. All HUD text migrated to `DrawTextEx` with `ui_font_for`. Decal layer chunking in `src/decal.c`: levels ≤4096 px keep single-RT M4 path; >4096 px partition into 1024×1024 chunks with `DECAL_MAX_CHUNKS=64` cap and lazy `ensure_chunk_alloc(cx, cy)` on first paint so untouched zones cost zero memory — typical Citadel-sized match stains 3–8 chunks = 12–32 MB peak inside the 80 MB texture budget. `decal_paint_blood` queues into pending list, `decal_flush_pending` walks chunks + finds overlapping splats + batches paints inside one `BeginTextureMode` pair per dirty chunk. New module `src/hotreload.{c,h}` — DEV_BUILD-gated mtime watcher polling every 250 ms (Makefile DBG_CFLAGS adds `-DDEV_BUILD`; release builds compile every public function as a no-op). main.c registers callbacks at startup for chassis × 5 atlases + weapons.png + decorations.png + hud.png + halftone_post.fs.glsl; on file change after the first sighting the callback fires `unload + (lazy)load`. Per-map kit textures reload when the map changes via `map_kit_load`. Resolves the M4 trade-off "Default raylib font (no vendored TTF)".
- [x] **P14** — Audio module. New `src/audio.{c,h}` (~700 LOC) with 47-entry SFX manifest + alias pool (3/5/8 aliases per fire-rate, round-robin rotation), 5 software buses (master/sfx/music/ambient/ui) with default mix `1.00/1.00/0.30/0.45/0.70`, listener-relative `audio_play_at` (200..1500 px linear attenuation, ±800 px pan; raylib's `SetSoundPan` mapping the [-1,+1] range to [1,0]), `audio_play_global` for spatial-irrelevant cues (UI clicks, capture, kill fanfare). `audio_request_duck` stacks via `min(duck_target)` on music + ambient for big detonations (≥100 dmg AOE in `explosion_spawn` → 0.5× for 300 ms recovery). Per-frame `audio_step` runs duck recovery + `UpdateMusicStream` (must run every frame to avoid streaming-buffer underrun) + ambient retrigger (raylib `Sound` has no native loop) + servo modulation from local-mech pelvis velocity (`pos - prev` × 60 Hz). Servo loop: target = `clamp(vel/280) × 0.7`, current lerps toward target at 0.15 per call; below 0.005 the volume floors to zero. Per-map music streaming via `LoadMusicStream` + `Music.looping = true`; ambient via small `Sound` retrigger; both routed through new `apply_audio_for_map(g)` in `start_round` resolving `level.meta.music_str_idx` + `ambient_loop_str_idx` from the level's STRT lump (assets/-prefixed for relative paths; same-map round-loop dedups). ~30 call sites wired: weapon SFX in `weapons_fire_hitscan` / `_lag_comp` / `weapons_predict_local_fire` / `weapons_spawn_projectiles` / `weapons_fire_melee` and both grapple branches in `mech.c`; SFX_HIT_FLESH on bone hit + SFX_HIT_CONCRETE on wall hit; SFX_EXPLOSION_LARGE/MEDIUM/SMALL bucketed by AOE damage in `explosion_spawn`; SFX_GRAPPLE_HIT on land in `projectile_step`; SFX_FOOTSTEP_CONCRETE in `build_pose`'s ANIM_RUN swing→stance gait wrap (new `Mech.gait_phase_l/_r` fields detect the wrap from >0.5 to <0.5 across ticks); SFX_JET_PULSE in `apply_jet_force` rate-limited every 4 ticks via new `Mech.last_jet_pulse_tick`; SFX_GRAPPLE_RELEASE in `mech_grapple_release`; SFX_KILL_FANFARE (`audio_play_global`) when killer is local + SFX_DEATH_GRUNT at victim pelvis in `mech_kill`; SFX_PICKUP_* per-kind in `pickup_step` on grab + SFX_PICKUP_RESPAWN for high-tier (POWERUP/WEAPON/ARMOR) on AVAILABLE rollover; SFX_FLAG_PICKUP/DROP/RETURN/CAPTURE in `ctf.c` (host) + mirrored in `client_handle_flag_state` via pre-decode snapshot/post-decode transition detect (so pure clients hear flag events too); SFX_UI_CLICK in `ui_button` + SFX_UI_TOGGLE in `ui_toggle`; client-side fire SFX in `client_handle_fire_event` gated on `!predict_drew` so self-LMB-active hitscan doesn't double-play; HIT_FLESH in `client_handle_hit_event`. Hot-reload: new `audio_reload_path` walks the manifest by path + drops aliases + reloads source + regenerates aliases; servo + music + ambient handled separately. `audio_register_hotreload` iterates the manifest into the existing P13 mtime watcher (47 SFX paths + servo = 48 entries; with P13's 9 entries that's 57, inside the 64-entry cap). Lifecycle: `audio_init(&game)` runs after `weapons_atlas_load` in both `main.c` and `shotmode.c`; `audio_step(&game.world, dt)` runs once per render frame at the top of the main loop (regardless of mode so music continues across mode transitions); `audio_shutdown` runs before `_exit` so SFX / music / ambient unload while the audio device is still alive (audio_shutdown does NOT call `CloseAudioDevice` — `platform_shutdown` owns that). No SFX assets ship at P14 — all 47 manifest entries fail to load on a fresh checkout (raylib emits FILEIO warnings; our INFO line follows), `audio_play_*` no-ops for those ids; the asset-generation pipeline at P15+ fills `assets/sfx/` + `assets/music/` + `assets/ambient/` without further runtime work. Protocol id stays `S0LG` (P14 is render-side / listener-side; no wire changes).
- [x] **P15** — Chassis sprite-atlas content pipeline + first approved Trooper atlas (`assets/sprites/trooper.png`), **revised mid-prompt to a Soldat-style "gostek part sheet" extraction path**. The AI-diffusion-canonical pipeline that P15 started with — programmatic/auto-dotted T-pose skeleton + style anchor → SDXL (Illustrious-XL) + lineart/super-robot LoRAs + ControlNet-Union promax + IP-Adapter PLUS + 30-step dpmpp_2m_sde → 17-tile crop — plateaued below the art bar over four parameter/prompt iterations: Illustrious-XL is fundamentally an anime base, IP-Adapter *style transfer* re-weights but can't override that prior, the one BattleTech-Battlemechs LoRA the art-direction doc names is SD1.5-architecture (won't load on SDXL), and a detailed-illustration crop renders as same-colour orange mush at the ~80 px the mech occupies on screen — the rigid 17-part renderer wants clean flat plates with hard outlines. **Shipping path** — `tools/comfy/extract_gostek.py` slices a *gostek part sheet* (one reference image — Trooper's is 2048×2048 — with all 22 body parts laid out flat-shaded + black-outlined + captioned in reading order, the layout Soldat's gostek model uses for per-part rigid skinning; see `wiki.soldat.pl/index.php/Mod.ini` and OpenSoldat's `client/GostekRendering.pas`) straight into the atlas: scipy connected-component detection of the 22 part shapes → band-cluster into reading rows → caption-sequence → `s_default_parts` slot → per-part rotate/flip to the vertical/horizontal slot (forearms `-90°`, head vertical-flip) → resize to the runtime sprite size → border-flood white-key (enclosed white panels survive) → optional 2-colour palette snap (Foundry by default; the runtime `halftone_post` shader screens at framebuffer res, so no Bayer is baked into the sprite) → composite at the same dst rects as `src/mech_sprites.c::s_default_parts` (no transcribe pass) → write `assets/sprites/<chassis>.png` + an annotated preview + a reproducibility manifest. `trooper.png` now comes from `tools/comfy/gostek_part_sheets/trooper_gostek_v1.png`. **Alternative path (kept, deprioritised)** — the AI-diffusion tools stay in `tools/comfy/`: `asset.py` orchestrator (skeleton → anchor → canonical → crop → install → verify), `run_workflow.py` ComfyUI API driver, `crop_canonical.py` (canonical → atlas; now also shared infra for the gostek path's per-tile post + palette tables), `transcribe_atlas.py` C-literal generator, `make_trooper_skeleton.py` (programmatic full-frame mech anatomy with helmet+visor / shoulder pads / panel-lined torso / gauntlets / boots / magenta pivot dots) + `prepare_skeleton.py` (auto-detects a bare mech silhouette and stamps the 16 pivot dots + bone axes from a chassis profile), `build_crop_table.py`, `contact_sheet.py` (candidate review); workflows `mech_chassis_canonical_v1.json` (full Pipeline 1, ≥12 GB) and `mech_chassis_canonical_8gb_v1.json` (8 GB subset — T2I-Adapter Lineart, no IP-Adapter, CPU VAE). Plus two render-side sprite-anchor fixes that benefit both paths: `draw_mech_sprites` scales 2-particle bone sprites to the actual bone length (authored `draw_h` was 2.4–5.7× the per-chassis bone span, so dense tiles overhung past the particles); foot pivot moved from sprite-center to bottom-edge so the boot sits above the FOOT particle = ground contact. New trade-off entry "Chassis art is hand-authored gostek part sheets, not the AI-diffusion pipeline" (replaces the short-lived 8 GB-VRAM entry); `assets/credits.txt` + `documents/art_log.md` carry the Trooper row. **P16 onward uses the gostek format** — author (or generate out-of-repo) one `<chassis>_gostek_v*.png` per chassis and run `extract_gostek.py`; whether the team keeps ComfyUI in the loop for *producing* those sheets, and what workflow does it best, is open.
- [x] **P16** — All 5 chassis atlases ship via the gostek part-sheet extraction path (`tools/comfy/extract_gostek.py` slices each `<chassis>_gostek_v*.png` from `tools/comfy/gostek_part_sheets/` into `assets/sprites/<chassis>.png`); weapon atlas (`assets/sprites/weapons.png`, 1024×256, all 14 slots) via new `tools/comfy/pack_weapons_atlas.py` (scipy CC detect with tone-band filter against a Perplexity checkerboard `#C9C9C9` + white "fake-transparent" background, reading-row sort + slot-order remap into the runtime sub-rect layout); HUD atlas (`assets/ui/hud.png`, 256×256, weapon-icon family + bar end-caps + crosshair + kill-flag glyphs + ammo-box frame); Foundry parallax kit (`assets/maps/foundry/parallax_{far,mid,near}.png`, all 3 layers). New pipeline step: `tools/comfy/keyout_halftone_bg.py` (scipy local-dark-fraction detector) keys out the opaque dithered canvas in `parallax_mid` + `parallax_near` so foreground silhouettes stay opaque and the rest becomes alpha=0 — required because `parallax_near` draws AFTER the world inside `renderer_draw_frame` at 95% scroll and would otherwise cover the chassis entirely. Originals backed up to `tools/comfy/raw_atlases/parallax_originals/`. One shotmode fix in `src/shotmode.c::shotmode_run`: call `map_registry_init()` before `parse_script` so the new `map <short>` directive resolves at parse time (`game_init` re-runs it idempotently). Four new shot tests verify the asset pipeline end-to-end: `tests/shots/m5_p16_foundry_full.shot` (parallax + chassis + HUD end-to-end on Foundry — keyout makes the world visible behind the layered industrial silhouettes), `m5_p16_chassis_lineup.shot` (all 5 chassis on a flat floor with 5 distinct weapons), `m5_p16_weapons_showcase.shot` (5 Trooper dummies cycling through Riot Cannon / Auto-Cannon / Pulse Rifle / Plasma Cannon / Microgun for the wider-silhouette half of `g_weapon_sprites[]`), `m5_p16_hud_smoke.shot` (HP/jet bars + armor pip + weapon-icon ammo + crosshair on a single Trooper). All editor shots still pass (`make test-editor` → 0 assertion failures). `TRADE_OFFS.md` deltas: "Mech capsule renderer is the no-asset fallback" entry **deleted** (5/5 chassis ship sprites in real play — the capsule path stays in source as a dev-machine convenience for builds without asset packs); "Per-map kit textures aren't on disk yet" entry **narrowed** to "Foundry only ships parallax (P16-partial)" with the keyout step documented. Per-map parallax kits for the 7 other maps (Slipstream / Concourse / Reactor / Catwalk / Aurora / Crossfire / Citadel) remain pending as P16-followup work.
- [x] **P17** — Author maps 1-4 as `.lvl` files via `tools/cook_maps/cook_maps.c` (replaces the P01 stub; `make cook-maps` builds + runs it, each emitted file round-trips through `level_load` + `level_build_poly_broadphase` inside the cooker via a `verify` arena). Output: `assets/maps/{foundry,slipstream,reactor,concourse}.lvl`. Per-map summary: **Foundry** (100×40, FFA|TDM, 16645 B) — 45° spawn ramps + 30° center hill + 2 edge alcoves + 8 spawns + 12 pickups; **Slipstream** (100×50, FFA|TDM, 20758 B) — 60° basement slide chutes + ICE catwalk patches + 3 angled struts + 4-room basement cave network + 2 WIND zones + 8 spawns + 14 pickups; **Reactor** (110×42, FFA|TDM, 19233 B) — 30° bowl floor + 45° pillar-underside overhangs + flanking ramps + 2 spawn alcoves + 8 spawns + 16 pickups; **Concourse** (100×60, FFA|TDM, 24965 B) — programmatic scaffold (the brief expects editor-authoring; tracked as a TRADE_OFFS entry "Concourse is synthesized programmatically, not editor-authored"), two 30° hills + 4 cover columns + 2 FOG zones + 4 alcoves + 16 spawns + 18 pickups. `.gitignore` narrowed so the four authored `.lvl` files track while scratch/editor fixtures stay ignored. `tests/net/run_3p.sh` switched from Foundry to Reactor per spec (10/10 PASS). Also two cross-build sync fixes that landed under this prompt: **sync-fix** (snapshot_apply now re-syncs `armor_id` + `jetpack_id` + `fuel_max` + `armor_hp_max` every tick — pre-fix these were set only at spawn, so a bad first snapshot stuck the client's mech with wrong caps for the entire round; alignment of `ensure_mech_slot` clamp defaults with `server_handle_lobby_loadout`'s clamps removes silent client↔server divergence on out-of-range values) and **map-sync** (`map_alloc_tiles` and `level_build_tutorial` now `memset(Level, 0, sizeof *Level)` at the top, matching what `level_load` already does at `src/level_io.c:504` after parsing the .lvl header; pre-fix the code-built fallback path inherited `poly_count` / `pickup_count` / `*_pointers` from whatever map was last loaded into this World, and the stale pointers aliased freshly-reset arena memory — producing ghost polygons + ghost pickups on the same `map_id` between two clients with different lobby-cycle histories). `DIAG-sync:`-prefixed `LOG_I` lines now tag every loadout-sync and map-build seam (`lobby_add_slot` / `server_handle_lobby_loadout` / `lobby_spawn_round_mechs` / `ensure_mech_slot` / `snapshot_apply re-sync` / `map_build` result / `map_build_for_descriptor` decision) so paired host/client logs make divergence immediately obvious; a `local_mech_id stuck` 2 s detector fires if the late-bind ever fails to resolve.
- [ ] **P18** — Author maps 5-8 (Catwalk / Aurora / Crossfire / Citadel) + bake-test harness
- [ ] **P19** — Audio assets (~47 SFX + per-map music + ambient loops) against the P14 runtime manifest

## wan-fixes log

Off-roadmap WAN-play polish iterations, shipped between P16 and P17
based on a real two-laptop bake test (MN ↔ AZ) and a follow-up
local-LAN test session.

| Tag | What | Why |
|-----|------|-----|
| **wan-fixes-1** (PR #19) | Server-side hitscan lag-comp + 60 Hz snapshots + redundant input packets + grapple-head lag-comp | M2 baseline was 30 Hz snapshots + no lag-comp on the grapple head; remote shots felt 100–200 ms late at WAN ping. |
| **wan-fixes-2** (PR #20) | Bundle `assets/` into the artifact zip + restore 100 ms render interp | First MN ↔ AZ test launched the .exe without assets so atlases/SFX/fonts all hit the no-asset fallbacks; interp had crept to 0 in an earlier patch. |
| **wan-fixes-3** (PR #21) | Remote mechs are now **kinematic** on the client (`inv_mass=0`); only `snapshot_interp_remotes` moves them | Pre-fix the client ran full pose-drive + 12-iter solver on remote bones with stale `latched_input`, producing visible wobble. |
| **wan-fixes-4** (PR #22) | `SNAP_STATE_RUNNING` wire bit + gait-lift hysteresis | Remote walk animation flickered between Run and Idle each tick because the gait detector ran against pelvis velocity only. |
| **wan-fixes-5** (PR #23) | "Host Server" now spawns a child `--dedicated PORT` Soldut and the host UI joins as a regular client | Listen-server host had asymmetric latency (zero on host, full RTT on joiner). Dedicated child gives both sides the same client pipeline. |
| **wan-fixes-6** (PR #24) | Forward host setup (mode / map / score / time / ff) to the dedicated child + in-lobby host controls | The dedi child started with `soldut.cfg` defaults regardless of what the host UI picked. |
| **wan-fixes-7** (PR #25) | Re-publish cached UI loadout on re-host | After disconnect + re-host, the lobby UI showed the previous draft but `round_start` spawned the default mech. |
| **wan-fixes-8** (PR #26) | Persist player prefs to `soldut-prefs.cfg` | Loadout / name / team / last-connect-addr now survive cold launches; key=value file next to the binary. |
| **wan-fixes-9** (PR #27) | Async "Starting server..." overlay on Host Setup | Synchronous spawn + 5 s connect-poll froze the window with no feedback. |
| **wan-fixes-10** (PR #28) | Frag-grenade sync fix (server-driven `NET_MSG_EXPLOSION` event) + lobby loadout-snap clobber | Client's visual grenade detonated against rest-pose remote bones (per wan-fixes-3) ~9 px off from the server's animated bones; sync_loadout_from_server also silently reverted prefs to defaults on every fresh connect. |
| **wan-fixes-11** (PR #29) | Lobby polish — `<` / `>` arrows on cycle buttons (RMB walks backward), Ready Up gets pulsing-green chrome, "Loading match..." overlay between countdown end and first snapshot | Cycle buttons were LMB-only forward; Ready button shared chrome with the loadout row above; the COUNTDOWN→ACTIVE gap had no visual feedback. |
| **wan-fixes-12** (PR #30) | Text-input cursor (`Left / Right / Home / End / Delete`, mid-string insertion) | Name + direct-connect + chat fields were append-only with EOL-only Backspace. |
| **wan-fixes-13** (PR #31) | Kill feed wire flags + names end-to-end, top-right corner with banner-aware layout, 15 s linger with 3 s fade-tail, **default window 1920×1080** | Client never populated `world.killfeed[]`, wire dropped the flag byte (no headshot/gib/overkill icons), HUD rendered "mech#N". |
| **wan-fixes-14** (PR #32) | Loadout + team picker lock once Ready Up is committed | You could ready up and keep tweaking weapons; `round_start` used whatever the slot held when ready latched. |
| **wan-fixes-15** (PR #33) | HP numerals 18 → 22 px, armor bar lifted 10 px above the text top | Armor bar's bottom edge clipped the top of the HP digits by 4 px. |
| **wan-fixes-16** (PR #37) | "Host Server" UI runs the dedicated server in a separate **thread** of the host UI process instead of spawning a child. | Windows silently drops UDP between any pair of processes that share a spawn-tree ancestor — confirmed on two machines through five increasingly aggressive workarounds (handle inheritance off, breakaway from job, launcher-pattern PPID severance, parent-spoofing to Explorer). Same-process UDP loopback works fine, so the server moves into a thread of the UI process. Two `Game` structs share zero memory — they communicate strictly over UDP loopback, host has no zero-latency advantage. |

Five new wire messages cumulatively (`NET_MSG_EXPLOSION` for AOE
events; the kill-event payload widened from 7 to 39 bytes for flags
+ names; the input payload bundles last-N inputs as redundancy;
snapshot frequency bumped 30 → 60 Hz; map-share kept at P08's four
existing tags). Protocol id stays `S0LG` (all additions are
length-prefixed or gated on existing state bits).

Three new regression test runners shipped alongside:
`tests/shots/net/run_frag_grenade.sh` (paired-dedi grenade sync,
both throw directions), `tests/shots/net/run_kill_feed.sh`
(end-to-end kill-event flags + names), and the `make
host-overlay-preview` / `make lobby-overlay-preview` visual smoke
binaries for the new spinners.

User-visible state files written next to the binary now include
`soldut.cfg` (server defaults), `bans.txt` (P09 ban list), and
`soldut-prefs.cfg` (wan-fixes-8 per-user picks). All cwd-relative
so they live next to `Soldut.exe` / `Soldut.app` in the artifact
zip. `.gitignore` covers the last two.

## License

The game is MIT-licensed (see `LICENSE` once added). Vendored dependencies
keep their own licenses:

- raylib — zlib/libpng
- ENet — MIT-style
- stb_ds.h, stb_sprintf.h — public domain (Sean Barrett)
