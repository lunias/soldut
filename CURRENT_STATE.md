# CURRENT_STATE — Where the build actually is

A living snapshot of debugging and playtesting. Updated as the build
moves. The design documents in [documents/](documents/) describe the
*intent*; this file describes the *current behavior* of the code that's
sitting on disk right now.

Last updated: **2026-05-04** (M4 lobby & matches in; M5 P01 — `.lvl` format + loader/saver, P02 — polygon collision + slope physics, P03 — render-side accumulator + interp alpha + reconcile smoothing + two-snapshot remote interp + `is_dummy` bit, and P04 — standalone level editor at `tools/editor/` + `--test-play` flag in the game in).

---

## Milestones

| Milestone | Status                                  |
|-----------|-----------------------------------------|
| **M0**    | Done — see [README.md](README.md).      |
| **M1**    | Playable end-to-end. B1, B3, B4, B5, B6, B7 fixed 2026-05-03. Close-delay fixed same day. |
| **M2**    | Foundation lands 2026-05-03. Host/client handshake works locally; per-tick input ship + 30 Hz snapshot broadcast + client-side prediction & replay + per-mech bone history for hitscan lag compensation are wired. LAN-only, full snapshots, no mid-tick interpolation of remote mechs (see TRADE_OFFS.md). Two-laptop bake test still pending. |
| **M3**    | Combat depth in 2026-05-03. All 5 chassis (Trooper / Scout / Heavy / Sniper / Engineer) with passives. All 8 primaries (Pulse Rifle, Plasma SMG, Riot Cannon, Rail Cannon, Auto-Cannon, Mass Driver, Plasma Cannon, Microgun) and 6 secondaries (Sidearm, Burst SMG, Frag Grenades, Micro-Rockets, Combat Knife, Grappling Hook). Projectile pool with bone + tile collision. Explosions: damage falloff, line-of-sight check, impulse to ragdolls. Per-limb HP and dismemberment of all 5 limbs. Recoil + bink + self-bink fully wired. Friendly-fire toggle (`--ff` server flag). Kill feed with HEADSHOT/GIB/OVERKILL/RAGDOLL/SUICIDE flags. Loadout via CLI flags (`--chassis`, `--primary`, `--secondary`, `--armor`, `--jetpack`). Snapshot wire format widened to carry chassis/armor/jet/secondary; protocol id bumped to `S0LE`. |
| **M4**    | Lobby & matches in 2026-05-03. Game flow is now title → browser → lobby → countdown → match → summary → next lobby. New modules: `match.{h,c}`, `lobby.{h,c}`, `lobby_ui.{h,c}`, `ui.{h,c}` (small immediate-mode raylib UI helpers, scale-aware for 4K), `config.{h,c}` (`soldut.cfg` key=value parser), `maps.{h,c}` (Foundry / Slipstream / Reactor — three code-built maps for the rotation; `.lvl` loader is M5). LOBBY-channel messages (player list with `mech_id`, slot delta, loadout, ready, team change, chat, vote, kick/ban, countdown, round start/end, match state). Server config file: port, max_players, mode, score_limit, time_limit, friendly_fire, auto_start_seconds, map_rotation, mode_rotation. Single-player flow auto-hosts an offline server and arms a 1s countdown. Protocol id bumped `S0LE` → `S0LF`. Network test scaffold under `tests/net/` runs the host/client end-to-end via real ENet loopback and asserts on log-line milestones. |
| **M5**    | In progress. **P01–P04** in. P02: per-particle contact normals (Q1.7 SoA fields + `PARTICLE_FLAG_CEILING`); polygon broadphase grid built at level load (`level_build_poly_broadphase`); `physics_constrain_and_collide` interleaves tile + polygon collision per relaxation iter (closest-point-on-triangle, push-out via pre-baked edge normals); `level_ray_hits` tests segments against polygons too; slope-tangent run velocity, slope-aware friction (`0.99 - 0.07*|ny|`, ICE→0.998), angled-ceiling jet redirection, slope-aware post-physics anchor (skips when `ny_avg > -0.92`); WIND/ZERO_G ambient zones; environmental damage tick for DEADLY tiles+polys+ACID zones. Renderer draws polygons (P02 stopgap, replaced by sprite art at P13). P03: per-particle `render_prev_x/_y` snapshot at the top of each `simulate_step`; renderer lerps `pos` ↔ `render_prev` by `alpha = accum/TICK_DT` (also threaded through projectile + FX draw); reconcile `visual_offset` is now read by `renderer_draw_frame` and applied additively to local-mech draws (decays over ~6 frames so server snaps don't read as glitches); per-mech remote snapshot ring (`remote_snap_ring[3]`) + `snapshot_interp_remotes` lerps remote mechs at `client_render_time_ms - 100ms` between bracketing entries (clamped to nearest if only one entry, snap+clear on >200 px corrections); `state_bits` widened u8→u16, `SNAP_STATE_IS_DUMMY` rides bit 11 so client dummies don't drive arm-aim; protocol id bumped `S0LF` → `S0LG`. **Pending**: P04 (level editor), P05+ (pickups, grapple, CTF, map sharing, controls, art, audio, maps). |

---

## Game flow (M4)

`./soldut` opens at the **title screen**. Main paths:

- **Single Player** — bootstraps an offline-solo "server" (no
  network), seats the player in slot 0, and arms a 1-second
  auto-start. Round runs against an empty map; useful for testing
  movement/jets in isolation. (A practice dummy is M5 alongside
  pickups.)
- **Host Server** — opens a real ENet server on `cfg.port` (default
  23073), seats the host in slot 0, sits in the lobby waiting for
  joiners. Auto-arms the round countdown when ≥2 active slots are
  filled (or a host clicks "Start now").
- **Browse Servers** — broadcasts a `DISCOVERY_QUERY` to
  255.255.255.255:DEFAULT_PORT+1; servers reply with a
  `DISCOVERY_REPLY` carrying name + port + player count. Refresh
  every 5 s (or via the Refresh button). Click a row → Connect.
- **Direct Connect** — text input for host:port → connect.

CLI shortcuts (skip title screen):

- `./soldut --host [PORT]` — go straight to host lobby.
- `./soldut --connect HOST[:PORT] [--name NAME]` — go straight to
  client lobby.
- `./soldut --chassis Heavy --primary "Mass Driver" ...` — pre-fill
  the loadout for the local slot.

Server config: drop a `soldut.cfg` next to the binary —
key=value pairs (port, max_players, mode, score_limit, time_limit,
friendly_fire, auto_start_seconds, map_rotation, mode_rotation).
CLI flags override the file.

## Networking (M2 + M4)

Connection flow:
ENet handshake → `CONNECT_REQUEST` → `CHALLENGE`(nonce, token) →
`CHALLENGE_RESPONSE` → `ACCEPT`(client_id, **slot_id**) →
`INITIAL_STATE`(lobby table + match state). Client transitions to
`MODE_LOBBY`. Mechs aren't spawned until `LOBBY_ROUND_START`
broadcasts; from then on the 30 Hz snapshot stream flows on the
`STATE` channel.

LOBBY-channel messages (`net.h` enum 20–33):
- `LOBBY_LIST` — full slot table (server → all)
- `LOBBY_SLOT_UPDATE` — one slot delta
- `LOBBY_LOADOUT` — pick chassis/weapons/armor/jet (client → server)
- `LOBBY_READY` — toggle ready
- `LOBBY_TEAM_CHANGE` — pick team (FFA forces team 1)
- `LOBBY_CHAT` — bidirectional, server scrubs + rate-limits
- `LOBBY_MAP_VOTE` — pick A/B/C
- `LOBBY_VOTE_STATE` — server → client current tally
- `LOBBY_KICK` / `LOBBY_BAN` — host-only commands
- `LOBBY_COUNTDOWN` — server → client auto-start tick
- `LOBBY_ROUND_START` / `LOBBY_ROUND_END` — phase transitions
- `LOBBY_MATCH_STATE` — current MatchState (mode/map/limits)

The first `--host` + `--connect 127.0.0.1` round-trip on the same
machine completes in <100 ms in playtest. Two mechs in one window
(the host's keyboard mech + the remote mech driven by the second
process's input) are simulated authoritatively on the host side and
streamed to the client at 30 Hz.

## M3 combat depth — what's wired

The full combat layer per documents/04-combat.md ships at M3. Highlights:

- **5 chassis** with parameterized stats. Pick on the local mech with
  `--chassis Heavy` (or Scout / Trooper / Sniper / Engineer). Each
  chassis carries a passive — Trooper: -25% reload time; Scout: BTN_DASH
  one-shot horizontal burst on grounded; Heavy: -10% explosion damage;
  Sniper: steady (placeholder for crouched-spread reduction); Engineer:
  BTN_USE drops a 50-HP self-heal on a 30s cooldown (proper "deployable
  pack" lands at M5 with the pickup system).
- **14 weapons** (8 primaries + 6 secondaries) in `src/weapons.c`'s
  `g_weapons[]` table. Hitscan, projectile, spread (pellets), burst
  (auto-fire N rounds), throw (grenades), melee (knife backstab x2.5),
  and grapple (stub — see TRADE_OFFS).
- **Projectiles** in their own SoA pool (`src/projectile.{h,c}`,
  PROJECTILE_CAPACITY=512). Per-tick gravity, drag, swept tile
  collision, swept bone collision against every other mech. Bouncy
  flag for grenades. AOE projectiles trigger explosion_spawn on hit
  or fuse expiry.
- **Explosions** apply damage with `1-(d/r)^2` falloff, halve damage
  through walls (LOS ray), apply impulse to every particle in radius
  (alive *and* corpse), spawn 28 sparks fan-out + screen shake.
- **Per-limb HP** (`hp_arm_l/_r, hp_leg_l/_r, hp_head`). When a limb
  drops to zero, `mech_dismember` deactivates the constraints between
  the limb's particles and the rest of the body. Limbs keep
  Verlet-integrating as gibs. Head detach is also lethal.
- **Bink + self-bink**: each weapon has `bink` (rad jolt to nearby
  targets per shot) and `self_bink` (rad jitter on shooter per shot).
  `mech.aim_bink` accumulates and decays exponentially each tick. The
  fire path rotates `mech_aim_dir` by `aim_bink` before tracing.
- **Friendly-fire toggle** at `world.friendly_fire` (default false,
  CLI `--ff`). Damage path drops same-team hits when off. Self-damage
  always goes through (Mass Driver self-splash is intentional).
- **Kill feed** ring buffer (5 entries) on World, drawn top-right by
  HUD with HEADSHOT / GIB (≥2 limbs) / OVERKILL (>200 dmg) / RAGDOLL
  (midair kill) / SUICIDE flags. Single-line center banner kept for
  shot-mode clarity.
- **Armor**: 4 variants (None / Light / Heavy / Reactive). Bullet
  damage flows through armor first at `absorb_ratio` until armor HP
  drains; then armor falls off. Reactive eats one explosion fully and
  breaks. Heavy plating costs 10% movement + jet.
- **Jetpacks**: 4 modules (Standard / Burst / Glide / JumpJet).
  Burst dumps 30% fuel for 0.4s × 2 thrust on BTN_DASH. Glide gives
  small lift at empty fuel. JumpJet replaces continuous thrust with
  re-jumps on BTN_JET (consumes a fuel chunk per).

Verification: see `tests/shots/m3_*.shot` — Mass Driver, Plasma SMG,
Frag Grenade, Riot Cannon spread, Rail Cannon charge, and an Armor
test all run through the real renderer + sim and dump PNG + log
pairs. The Mass Driver shot lands a one-rocket kill on the dummy with
a visible body launch off the platform; the Frag Grenade test shows
the projectile lobbing, bouncing, and detonating; Plasma SMG shows
40+ projectiles in flight over a single trigger hold.

## What works

A fresh checkout, `make`, `./soldut` opens a 1280×720 window onto the
tutorial level. From there:

- **Stand** — Pelvis pinned to standing height when grounded; spine,
  head, knees, feet all stable. No sag, no fold-over, no oscillation.
  The 120-tick idle test (`./build/headless_sim`, Phase 2) shows pelvis
  Y holding its spawn value to floating-point precision and Y-velocity
  reading 0.00.
- **Run** — `A`/`D` accelerates the whole body to ~280 px/s on the
  ground, ~98 px/s in the air (35% air control). Active braking when
  grounded with no input gives a snappy stop.
- **Jump** — `Space` applies a vertical impulse of 320 px on a fresh
  ground contact; locked out until next ground touch.
- **Jet** — `W` thrusts at 2200 px/s². Drains fuel at 0.6/sec. Regens at
  0.2/sec when grounded. Feels good in fullscreen; see "vsync /
  frame-rate" in [TRADE_OFFS.md](TRADE_OFFS.md) for the windowed-mode
  caveat.
- **Aim** — Crosshair follows the mouse in world space. Right arm pose
  is built from the aim vector each tick; the rest of the body is
  layout-consistent.
- **Fire** — `LMB` shoots the Pulse Rifle: 18 dmg, 110 ms cycle, 1.5 s
  reload, 30-round mag, 2400 px range. Tracer renders, recoil impulse
  pushes the chest back. Hitscan vs bone capsules.
- **Dummy** — Yellow target at (2400, 984). Stands stable (same anchor
  as the player). Takes damage. At 0 HP it transitions to ragdoll —
  pose drive drops and gravity does the rest. Left arm comes off when
  its limb HP drops to 0; the constraint connecting it to the shoulder
  is deactivated and the limb keeps Verlet-integrating as a free piece.
- **Blood / decals** — Blood particles spawn on hit, fall, and splat
  onto the persistent decal RT. Splats survive forever and stack.
- **Hit-pause** — A handful of ticks of frozen physics on a notable kill
  (the killshot itself, not chip damage). FX (blood) keeps falling
  during the freeze.
- **Screen shake** — Decays exponentially. Triggered by recoil and
  notable events.
- **HUD** — Health bar, jet fuel gauge, ammo counter, crosshair, kill
  feed. Drawn last, on top of everything.

The 30-second loop the M1 milestone calls for ("run, jet, shoot the
dummy until it falls apart — and grin") is reachable.

---

## Network smoke tests (M4)

`tests/net/run.sh` and `tests/net/run_3p.sh` spawn `./soldut --host`
and `./soldut --connect` as background processes against a real
loopback ENet socket. They wait for the lobby/match flow to play
through (auto_start → countdown → round → end) and assert on key
log-line milestones — e.g. "client resolves local_mech_id → 1".
The scripts always reap their child processes via a `trap cleanup
EXIT` so a half-finished test never leaves an orphan window on the
desktop.

Run:
```
./tests/net/run.sh        # 1 host + 1 client, FFA, 1 round → 13 assertions
./tests/net/run_3p.sh     # 1 host + 2 clients, TDM, 1 round → 10 assertions
./tests/net/run.sh -k     # keep tmp dirs (/tmp/soldut-net-XXX) for inspection
```

This is the test that caught the M4 black-screen-on-client bug
(`lobby_decode_list` was clobbering the just-decoded `mech_id`
back to -1 — see "Recently fixed" below).

## Networked SHOT tests (paired host+client screenshots)

`tests/shots/net/run.sh <name>` orchestrates a host shot script
+ a client shot script, both connecting via ENet loopback, both
writing PNG screenshots from their own perspective. Lets you
visually verify what each player's screen looked like at the
same moment.

Available test pairs:
- `2p_basic`     — sit in lobby + match + summary; FFA, no movement
- `2p_motion`    — host walks RIGHT + jets, client walks LEFT, they
                   meet near map center (verifies snapshots flow)
- `2p_countdown` — captures the lobby with countdown banner active
                   for host vs client comparison
- `ui_audit`     — clean screenshots of lobby / match / summary at
                   1280×720 for visual regression review

Outputs land in `build/shots/net/{host,client}/<name>.png` plus a
contact-sheet `sheet.png` that composes all named screenshots into
one image. The shot script syntax adds `network host PORT` and
`network connect HOST:PORT` directives to the existing shotmode
grammar — see `src/shotmode.h` for the full reference.

Run:
```
./tests/shots/net/run.sh 2p_basic
./tests/shots/net/run.sh 2p_motion
./tests/shots/net/run.sh ui_audit
```

The driver always reaps its children, so test runs don't leave
orphan windows.

## M5 progress

Tracking what's landed vs what's pending in the M5 (Maps & content)
milestone. Work is sequenced through `documents/m5/prompts/`.

### Done

- **P01 — `.lvl` binary format + loader/saver + tests** (2026-05-03).
  - New module `src/level_io.{h,c}`. Public API: `level_load(World*,
    Arena*, path)`, `level_save(const World*, Arena*, path)`,
    `level_crc32(...)`. Endian-explicit `r_u16/u32/i16` + `w_u16/u32/i16`
    helpers; little-endian host enforced via `#error`.
  - File layout per `documents/m5/01-lvl-format.md`: 64-byte header +
    Quake-WAD-style lump directory + 9 lumps (TILE / POLY / SPWN / PICK
    / DECO / AMBI / FLAG / META / STRT). CRC32 (poly 0xEDB88320,
    table-based) over the whole file with the CRC field zeroed during
    compute.
  - `Level` struct migrated from `uint8_t *tiles` to `LvlTile *tiles`
    (4 bytes per cell — id + flags). All Lvl* record types added to
    `world.h` with `_Static_assert` size locks (4/32/8/12/12/16/8/32).
    Existing physics/render reads continue to work via `level_tile_at`
    which now branches on `flags & TILE_F_SOLID`.
  - `map_build` is now `(MapId, World*, Arena*)` and tries
    `assets/maps/<short>.lvl` first; falls back to the M4 code-built
    path on any `level_load` failure. "File not found" logs at INFO
    (expected until P17); other failures (`BAD_CRC`, `BAD_DIRECTORY`,
    etc.) log at WARN.
  - `make test-level-io` builds + runs `tests/level_io_test.c`:
    synthetic World with one of every section → save → reload →
    re-save → byte-compare both files. Plus bit-flip → expect
    `LVL_ERR_BAD_CRC`; truncate-to-half → expect `BAD_DIRECTORY`,
    `BAD_SECTION`, or `BAD_CRC` (don't crash). 25/25 passing on
    first ship.
  - `tools/cook_maps/cook_maps.c` exists as a stub. P17 fills it in
    by allocating a synthetic World per code-built map and calling
    `level_save` to `assets/maps/`.

- **P02 — polygon collision + slope physics + slope-aware anchor** (2026-05-03).
  - `ParticlePool` extended with per-particle contact data:
    `contact_nx_q` / `contact_ny_q` (Q1.7 normals) and `contact_kind`
    (TILE_F_* bitmask). New `PARTICLE_FLAG_CEILING`. Cleared at the
    start of `physics_apply_gravity`; written by every contact in
    `contact_with_velocity` / `contact_position_only`.
  - `level_io.c` builds the polygon broadphase grid
    (`Level.poly_grid` / `poly_grid_off`) at load time. Two passes over
    the polygon list: count per-cell, then prefix-sum + write. The
    helper is exposed as `level_build_poly_broadphase` for code-built
    maps to call after populating `polys`. The `Level` struct gains a
    `struct Level` tag so other headers can forward-declare it.
  - `physics_constrain_and_collide` interleaves `solve_constraints`,
    `collide_map_one_pass`, `collide_polys_one_pass` per iteration
    (12 iters). Inline closest-point-on-triangle (Eberly form) plus
    a separate "particle inside triangle → push out via nearest edge"
    branch. Polygon kinds map to TILE_F_* via
    `poly_kind_to_contact_kind` (ICE/DEADLY/ONE_WAY/BACKGROUND).
  - `level_ray_hits` now tests against polygons too (segment-vs-edge
    intersection per polygon, take min `t` across all polygons + DDA).
    BACKGROUND polygons skipped.
  - `apply_run_velocity` projects the run target onto the slope
    tangent. Sign chosen by run direction; X set on every particle, Y
    set on lower chain (knees + feet) only so the upper body keeps
    its gravity component. No-input + flat-ground = M1 active braking;
    no-input + sloped = skip (lets passive slide work).
  - `apply_jet_force` — particles with `PARTICLE_FLAG_CEILING` redirect
    the upward thrust along the ceiling tangent, sign picked by run
    input. Other particles get the existing straight-up impulse.
  - `mech_post_physics_anchor` early-outs when the average foot
    contact normal is more than ~22° off straight up (`ny_avg >
    -0.92`). Lets slope physics drive the pose on slopes; flat ground
    still gets the M1 standing anchor.
  - `physics_apply_ambient_forces` (called from
    `physics_apply_gravity`) handles WIND zones (per-tick Verlet prev
    nudge) and ZERO_G zones (per-particle skip mask). FOG is render-
    only.
  - `mech_apply_environmental_damage` (called from `simulate_step`)
    applies 5 HP/s to PART_PELVIS when any particle is on a DEADLY
    tile, inside a DEADLY polygon (broadphase + point-in-triangle), or
    inside an ACID ambient zone. Server-only.
  - Renderer draws polygons (filled + edge outline, color by kind) so
    shot tests show the slope test bed. Proper polygon art + halftone
    lands at P13.
  - Test scaffolding: `level_build_tutorial` carries three temporary
    SOLID slope polys (45°/60°/5° at floor-row mid-map) for
    `tests/shots/m5_slope.shot` to land on. Will be removed when P17
    ships authored `.lvl` maps. `make test-physics`, `make
    test-level-io`, and `tests/net/run.sh` all pass post-P02.

  **Tuning caveat**: the starting friction values from the spec
  (`0.99 - 0.07*|ny|`, range [0.92, 0.99]) leave a 60° slope close to
  the equilibrium where a planted foot doesn't passively slide as
  dramatically as the spec's English text describes. The slope physics
  pipeline is wired end-to-end (broadphase + closest-point + tangent
  projection + ceiling redirect + anchor gating); per-angle tuning is
  P02 §"Out of scope" → playtest after maps land.

- **P03 — render-side accumulator + interp alpha + reconcile smoothing
  + remote-mech interp + `is_dummy` bit + hit-event sync** (2026-05-04;
  three remote-interp bugs + blood/decal sync follow-up landed same day).
  - **Per-particle prev-frame snapshot.** `ParticlePool` gains
    `render_prev_x/_y` (parallel SoA arrays) plus matching fields on
    `ProjectilePool` and `FxParticle`. Seeded to current pos at
    `set_particle` / `projectile_spawn` / `fx_spawn_*` so first-frame
    reads aren't garbage. NOT to be confused with Verlet `prev_x/_y`
    — that's `pos - prev = velocity` and updates inside the
    integrator; `render_prev_*` only updates once per simulate tick.
  - **Snapshot at top of `simulate_step`.** One pass copies `pos →
    render_prev` for every live particle, projectile, and FX particle
    before any work (including the hit-pause early return path so the
    renderer always sees consistent anchors). ~30 µs / tick worst
    case.
  - **Renderer lerps everything.** `particle_render_pos(p, i, alpha)`
    helper in `render.c`; threaded through `draw_bone`, `draw_bone_chk`,
    `draw_mech`, `projectile_draw`, `fx_draw`. The `(void)alpha;`
    discard in `renderer_draw_frame` is gone; alpha is now used. Vsync-
    fast displays no longer accelerate physics; sim stays locked at
    60 Hz regardless of render rate. Camera follow is unchanged
    (it has its own smoothing).
  - **Reconcile visual offset** is now passed into `renderer_draw_frame`
    as `local_visual_offset` and applied additively only to the local
    mech's particles. The existing `reconcile_tick_smoothing` decay
    (~6 frames) hides server snaps. Host passes `(0,0)` (no
    reconcile state).
  - **Two-snapshot remote-mech interp buffer.** Per-`Mech`
    `remote_snap_ring[REMOTE_SNAP_RING=8]` of `{server_time_ms,
    pelvis_x/_y, vel_x/_y}`. `snapshot_apply` pushes to the ring for
    remote mechs (replacing the old M2 35% lerp); the local mech
    still snaps fully so reconcile can replay from a known anchor.
    Per-tick `snapshot_interp_remotes(world, render_time_ms)` finds
    the bracket pair around `render_time` and writes the lerped
    pelvis (translates all 16 particles + sets per-particle
    velocity). `NetState` carries `client_render_time_ms` (double
    precision so 0.67 ms/tick of integer truncation doesn't drift
    the client behind the server). Large corrections (>200 px —
    respawn, teleport) clear the ring and snap fully.

    **Three sync bugs found and fixed after first ship**:
    1. **Ring size 3 spans only ~66 ms** but `INTERP_DELAY_MS=100`
       meant render_time always fell BEFORE the oldest entry →
       clamped to oldest → mech jumped one snapshot per ~33 ms
       instead of interpolating. Grew ring to 8 (~231 ms span).
    2. **`(uint32_t)(TICK_DT * 1000.0)` truncates to 16** instead
       of 16.667, so render_time drifted 0.67 ms/tick = 40 ms/sec
       behind the server. After a few seconds the client was
       hundreds of ms behind. Switched render_time to `double`.
    3. **Init clamped render_time at 0** when `first_snap.server_time
       < INTERP_DELAY_MS`, losing the "100 ms behind" offset and
       making render_time track server_time directly (always
       clamped to NEWEST → frozen-then-jump motion). Now keep
       render_time as a double that can go negative; clamp to 0
       only at the cast boundary into pick_bracket.

    Also wired `snapshot_interp_remotes` into `shotmode.c`'s sim
    loop (it had its own loop separate from main.c, was bypassing
    the new path entirely). Verification: paired host+client shot
    test (`tests/shots/net/2p_sync.{host,client}.shot`) shows
    smooth interpolated motion across consecutive frames; existing
    `2p_meet`, `2p_jitter`, `m5_smoothing` shot tests pass.
  - **`SNAP_STATE_IS_DUMMY` (bit 11).** `EntitySnapshot.state_bits`
    widened u8 → u16. Wire size 27 → 28 bytes per entity. Server
    `snapshot_capture` sets the bit when `m->is_dummy`; client
    `snapshot_apply` mirrors it onto the local `m->is_dummy` so
    `build_pose`'s `if (!m->is_dummy)` arm-aim gate fires correctly
    on dummies that arrived via the wire.
  - **`NET_MSG_HIT_EVENT` for blood/decal sync.** Server-side
    `mech_apply_damage` now appends to a per-`World` `hitfeed[64]`
    queue; main.c's `broadcast_new_hits` drains the queue each tick
    and broadcasts each hit (victim_mech, hit_part, pos, dir, dmg)
    on `NET_CH_EVENT` (reliable). Client's
    `client_handle_hit_event` mirrors the same blood + spark FX
    spawn the server did (8 + dmg/16 blood, 4 + dmg/32 sparks at
    the actual hit point with the actual hit direction). Replaces
    the speculative blood-from-health-decrease in `snapshot_apply`
    which spawned at PART_CHEST with a facing-derived spray —
    visibly different from the server's view. Verified via
    `tests/shots/net/2p_hit.{host,client}.shot`: both views show
    matching blood at the head when the client headshots the host.
  - **Remote-mech leg animation (RUN derivation).** `snapshot_apply`
    only set `anim_id` to JET / STAND / FALL — never RUN — because
    server-side RUN is computed from `BTN_LEFT/RIGHT` input bits
    that don't ride the snapshot wire. Worse, `mech_step_drive`
    runs for every mech each tick (including remote ones on the
    client) and OVERWROTE anim_id back to STAND every tick from
    `in.buttons=0`. Result: client-side host mech rendered in
    static STAND pose (legs together) while sliding sideways at
    run speed → looked like skating, not walking. Fix:
    1. `snapshot_apply` derives RUN from `|vel_x| > 2 px/tick` for
       grounded mechs (well above incidental drift, well below
       intentional run at ~4.66 px/tick).
    2. `mech_step_drive`'s anim_id assignment now gates on
       `w->authoritative || mid == w->local_mech_id` — server
       always uses input-based path; client only for its own
       predicted mech; remote mechs on the client respect the
       snapshot value. Verified visually via
       `tests/shots/net/2p_legs.{host,client}.shot`: client view
       of host clearly shows stride pose (one leg forward, one
       back) during run, matches host's own view.
  - **`NET_MSG_FIRE_EVENT` for tracer/projectile sync.** Same
    pattern: per-`World` `firefeed[128]` queue; `record_fire(...)`
    helper appends from `weapons_fire_hitscan`,
    `weapons_fire_hitscan_lag_comp`, `weapons_fire_melee`, and
    `spawn_one_projectile` (so spread weapons emit one event per
    pellet); main.c's `broadcast_new_fires` drains and broadcasts
    on `NET_CH_EVENT` (reliable). Client's `client_handle_fire_event`
    spawns matching FX based on `weapon_def(weapon_id)->fire`:
    HITSCAN → tracer + sparks; PROJECTILE / SPREAD / BURST / THROW
    → projectile via `projectile_spawn` (visual-only because
    `w->authoritative=false` skips the damage path naturally) plus
    a short muzzle tracer; MELEE → swing tracer. Skips events where
    `shooter == local_mech_id` so we don't double up with the local
    predict path. Without this, remote players' shots were
    invisible on the client (only the local shooter's predict put
    FX on screen → asymmetric "host fires but client sees nothing"
    feel). Verified via `tests/shots/net/2p_combat.{host,client}.shot`
    — a comprehensive combat test (spawn near each other, both
    move, trade fire, kill, MVP banner): both views show tracers
    from each shooter, blood at hit positions, kill feed banner,
    and matching round-summary scoreboard.
  - Protocol id bumped `S0LF` → `S0LG`. Version string `0.0.6-m5`.
  - Verification: `make` clean, `make test-physics` runs without
    regression, `tests/net/run.sh` 13/13 passing, `tests/net/run_3p.sh`
    10/10 passing, `make test-level-io` 25/25 passing,
    `tests/shots/net/run.sh {2p_meet, m5_smoothing, 2p_sync,
    2p_hit, 2p_combat, 2p_jitter, 2p_legs}` all 12/12 passing each.

- **P04 — standalone level editor + `--test-play` game flag** (2026-05-04).
  - New `tools/editor/` with `main.c`, `doc.{c,h}`, `poly.{c,h}`,
    `undo.{c,h}`, `view.{c,h}`, `palette.{c,h}`, `tool.{c,h}`,
    `play.{c,h}`, `files.{c,h}`, `validate.{c,h}` + a `Makefile`.
    Top-level `make editor` delegates and produces
    `build/soldut_editor`.
  - Editor links a SUBSET of `src/` (arena, log, hash, ds, level_io)
    plus raylib + raygui (header-only, vendored at
    `third_party/raygui/raygui.h`). Does **not** link mech / physics /
    net / simulate. Inspect the linker line: 0 references to combat
    code from the editor.
  - `EditorDoc` mirrors `Level`; resizable arrays via `stb_ds`
    `arrput` (only place outside the runtime that uses stb_ds).
    `doc_load` / `doc_save` route through the runtime's `level_io`
    so the editor speaks the exact same on-disk format.
  - 7 tools (Tile / Polygon / Spawn / Pickup / Ambient / Deco / Meta)
    with a small vtable. Universal verbs (`Ctrl+Z/Y/S/O/N`, `F5`,
    `Space`-pan, `Ctrl`-scroll-zoom, `G`/`Shift+G` grids, `H`-help)
    handled in `main.c` before tool dispatch. Right-click on objects
    deletes the nearest within 24 px.
  - **Polygon ear-clipping** (Eberly form) in `poly.c`. Validates
    `≥3 verts`, `≥8 px edges`, no self-intersection, non-degenerate
    area; flips CCW→CW automatically before triangulation. Edge
    normals pre-baked in Q1.15 so the runtime never normalizes at
    load.
  - **Slope + alcove presets** (`palette.c`): `ramp_up_30/45/60`,
    `ramp_dn_30/45/60`, `bowl_30/45`, `overhang_30/45/60`,
    `alcove_edge` (4 pieces), `alcove_jetpack` (4 pieces),
    `alcove_slope_roof` (floor + 45° slope roof), `cave_block`
    (alias of edge alcove). Each preset emits 1–8 LvlPoly triangles
    via `poly_triangulate`.
  - **Undo/redo**: two stacks of 64 commands. Tile-paint commands
    batch into strokes (mouse-press to mouse-release); object commands
    are atomic. Big tile ops snapshot the whole grid before mutation
    via `undo_snapshot_tiles`. New actions clear the redo stack.
  - **Save-time validation** (`validate.c`): ≥1 spawn, CTF flag bases
    matched by team-sided spawns, polygons in-bounds and non-degenerate,
    pickups not inside SOLID tiles, META display name non-empty, **alcove
    sizing** per `documents/m5/07-maps.md` — pickups in enclosed
    neighborhoods (≥3 SOLID walls within 96 px) must have ≥3 tiles
    interior height × ≥2 tiles depth × ≥16 px wall clearance. Failures
    pop a raygui message-box modal listing each problem on its own line.
  - **F5 test-play** (`play.c`): saves to
    `$TMPDIR/soldut-editor-test.lvl`, then `posix_spawn`s
    `./soldut --test-play <abs_path>` (or `CreateProcess` on Windows).
    Editor stays interactive while the child runs.
  - **Game-side `--test-play <path>` flag** (`src/main.c`): forces
    LAUNCH_HOST + offline + skip_title; configures FFA / 60 s round /
    1 s auto-start; stashes the path on `Game.test_play_lvl`. Both
    `bootstrap_host`'s pre-build and `start_round`'s rebuild route
    through the new `map_build_from_path` (`src/maps.c`) which calls
    `level_load` directly (no MapId rotation).
  - **File picker is a raygui textbox modal**, not vendored
    `tinyfiledialogs`. The editor accepts `argv[1]` as an initial
    open path and auto-fills `assets/maps/scratch.lvl` for first
    Save. Logged as a trade-off (see `TRADE_OFFS.md`).
  - **4K / hi-DPI scaling** (post-ship polish, same day). Editor
    sets `FLAG_WINDOW_HIGHDPI` + bilinear-filters the default font.
    `editor_scale(GetScreenHeight())` mirrors the game's
    `ui_compute_scale` (1.0× at 720p → 3.0× at 4K, snapping in 0.25
    steps); a `UIDims` struct computed each frame holds scaled
    `left_w`/`right_w`/`top_h`/`bottom_h`/`row_h`/`font_*` values
    that every panel + palette + modal reads. raygui's
    `GuiSetStyle(DEFAULT, TEXT_SIZE, ...)` is bumped each frame to
    match. Initial window opens at 80% of the primary monitor
    (clamped to ≥1280×800), so a 4K monitor lands a usable
    ~3000×1700 window instead of a tiny 1280×800 fixed box.
    `files.c` and the meta modal scale themselves the same way.
    All panels use a dedicated high-contrast color set
    (`COL_TEXT` / `COL_TEXT_DIM` / `COL_TEXT_HIGH` / `COL_ACCENT`)
    so labels stay legible against the dark panel background.
  - **Keyboard-shortcut help modal** (same polish). Pressing `H`
    opens a scrollable two-column reference (key | description)
    with sections for Global / View / Tools / Tile / Polygon /
    Objects. Mouse wheel scrolls; `Esc` or `H` again closes.
    Replaces the old "log to console on H" stub.
  - **Bug-fix round** (post-ship, same day, prompted by playtest):
    five user-reported issues caught and patched, with regression
    tests for each.
    1. Tile-flag checkbox labels were rendering off the right edge
       of the panel (raygui's `GuiCheckBox` uses `bounds` as the
       checkbox square — the label trails it. Earlier code passed
       the full row width as bounds, sending labels offscreen).
       Fix: small-square bounds + auto-trailing label.
       (`editor_ui.c::ui_draw_tile_palette`)
    2. `H` key didn't close the help modal. raylib's
       `EndDrawing()` calls `PollInputEvents()` at its tail, so
       any `IsKeyPressed` check AFTER `EndDrawing` reads the
       NEXT frame's edge state. Fix: process modal key toggles
       at the very top of the frame, in the same input window
       the open-on-H check uses; new `ui_help_toggle()` helper.
       (`main.c`, `editor_ui.{c,h}`)
    3. Help modal body text was overlapping the footer hint at
       the bottom — `body_y1` didn't reserve footer space.
       Fix: `body_y1 = dlg_y + dlg_h - footer_h - pad`.
       (`editor_ui.c::ui_help_modal_draw`)
    4. Meta button only opened the modal on tool TRANSITION; if
       you closed meta and clicked the button again, no-op
       because `picked == active_tool`. Fix: `ui_draw_tool_buttons`
       now returns the actually-clicked tool (vs. -1 for no
       click), and `main.c` always opens meta on a real click.
    5. F5 test-play ignored the `.lvl`'s authored spawn points —
       `map_spawn_point` always returned the M4-era hardcoded
       `g_*_lanes`. Fix: when `level->spawn_count > 0`, prefer
       the authored spawns (FFA round-robin / TDM team affinity).
       (`src/maps.c::map_spawn_point`)
  - **Regression infrastructure for the bug fixes**:
    - `tests/spawn_test.c` (14 unit assertions on
      `map_spawn_point` covering FFA round-robin, TDM team
      affinity, team=0 wildcard, and the empty-spawns
      fallback). `make test-spawn`.
    - `tests/spawn_e2e.sh` — end-to-end pipe: editor shot
      mode authors a `.lvl` with a floor + raised platform
      and a spawn ON THE PLATFORM, `./soldut --test-play
      <lvl>` loads it, `soldut.log` records both the spawn
      coords AND the post-physics pelvis position. The test
      asserts both — `grounded=1` after settle proves the
      mech actually landed on the platform (not the bottom
      floor, which would happen pre-fix). `make test-spawn-e2e`.
    - `tools/editor/shots/bugs.shot` — covers bugs 1, 2, 3, 4
      via `tile_flags`, `toggle_help`, layout shots, and
      `click_tool_button meta` directives. New shotmode events:
      `EV_TOGGLE_HELP`, `EV_CLICK_TOOL_BUTTON`, `EV_TILE_FILL_RECT`.
      Asserts on `help_open` / `meta_open` state (new fields).
    - `tools/editor/shots/help_layout.shot` and
      `help_layout_4k.shot` for visual layout regressions.
    - `make test-editor` runs every editor shot script.
  - **Cross-platform editor build + CI artifact**: `cross-windows.sh`
    and `cross-macos.sh` both build `SoldutEditor.exe` /
    `SoldutEditor` alongside the game. `tools/editor/play.c`
    guards its `<windows.h>` include with `WIN32_LEAN_AND_MEAN /
    NOGDI / NOUSER` so raylib's `Rectangle` / `CloseWindow` /
    `ShowCursor` no longer collide with `wingdi.h` / `winuser.h`.
    `.github/workflows/ci.yml` now runs `make editor` on Linux
    + macOS and ships both binaries in each artifact bundle:
    `soldut-linux-x86_64`, `soldut-windows-x86_64`,
    `soldut-macos`. The headless tests
    (`test-physics` / `test-level-io` / `test-spawn`) also run on
    every Linux + macOS push.
  - **SHOT_LOG-gated test-play diagnostics**: when `--test-play`
    is on, `main.c` flips `g_shot_mode = 1` so the existing
    `SHOT_LOG()` macro fires. Adds a per-second pelvis-pos line
    in `main.c`'s MATCH branch and converts the per-slot lobby
    spawn line to SHOT_LOG. **Production play paths emit none
    of these lines** — the macro is a one-branch no-op when
    `g_shot_mode == 0`.
  - **Editor shot mode** (same day). New
    `tools/editor/shotmode.{c,h}` + a `--shot <script>` flag.
    Script grammar mirrors the game's: `at <tick> <directive>`
    with header lines for `window`, `out`, `ticks`, `panels`,
    `contact_sheet`. Drives the editor's doc / tool / undo APIs
    deterministically (so we don't have to synthesize raylib
    keyboard / mouse events), captures PNG screenshots at marked
    ticks, and runs assertions that fail the run with non-zero
    exit. `assert <field> <op> <value>` covers `polys / spawns /
    pickups / ambis / decos / flags / tiles_solid /
    validate_problems / active_tool / dirty`.
    UI rendering extracted into `editor_ui.{c,h}` (UIDims +
    COL_* + every panel and modal) so shots render the actual
    editor chrome — toolbars, palettes, status bar, help modal,
    meta modal — at any window size, not a stub. The full UI
    shows by default; scripts can pass `panels off` for a
    canvas-only frame. Contact-sheet directive composites all
    captured shots into one PNG (configurable `cols` /
    `cell <W> <H>`). Four regression scripts ship under
    `tools/editor/shots/`: `smoke.shot` (every primary verb +
    save/load round-trip + undo/redo + validation),
    `poly_triangulation.shot` (square/pentagon/L-shape ear-clip
    counts), `validate_failures.shot` (the validator catches
    bad inputs), `scaling_4k.shot` (verifies UI fits at 3000×1900).
    Run via `make editor-shot EDITOR_SHOT_SCRIPT=...`. All four
    pass with 0 assertion failures.
  - **`src/math.h` reorder** — `<math.h>` now precedes the raylib
    include so the editor's stricter expansion paths see fabsf /
    fmaxf / etc. as proper declarations rather than builtins. Game
    behavior unchanged; the headless and level-io tests still pass
    (25/25 + headless run-through).
  - Verification: `make` clean, `make editor` builds without errors,
    `make test-physics` and `make test-level-io` (25/25) pass,
    `tests/net/run.sh` 13/13 still passing. Editor opens an existing
    `.lvl` (load logged at INFO); `./soldut --test-play <lvl>` loads
    the supplied map and arms the auto-start countdown.

### Pending

- **P05–P14** — pickups, grapple, CTF, map sharing, controls, mech
  atlas runtime, weapon art, damage feedback, parallax / HUD / TTF /
  halftone / decal chunking, audio.
- **P15–P18** — ComfyUI asset generation + the 8 maps + bake test.

## Headless test

`tests/headless_sim.c` is a non-graphical scenario runner. It builds the
real `World`, runs `simulate()`, and dumps particle positions across
five phases:

1. **Spawn** — sanity check.
2. **Idle 2 s** — does the body fold under gravity + pose alone? No.
3. **Hold RIGHT 1 s** — does the mech move? Yes, ~280 px/s, body upright.
4. **Release** — does it stop? Yes, vel ≈ 0 in a tick or two.
5. **Hold JET 0.5 s** — climb rate, body shape under thrust.

Run: `make test-physics && ./build/headless_sim`. This is how we
catch physics regressions without firing up a window.

---

## M1 close-out blockers

Found via shot-mode runs of `tests/shots/m1_*.shot` on 2026-05-03.
PNGs referenced live under `build/shots/<script>/`.

### B1 — Crumpled-leg landing while running (FIXED 2026-05-03)

Repro: run off the spawn platform with `right` held throughout the
fall (`tests/shots/b1_dense.shot`). Pre-fix, the body landed at
tick ~100 in a permanently folded pose — knees buckled inward, body
sunk close to the floor — and stayed crumpled indefinitely
(`settled_t200` was identical to `land_t100`). Releasing `right`
mid-air made the body land cleanly (`tests/shots/b1_release.shot`),
which pinned the cause: `mech_post_physics_anchor` was gated on
`anim_id == ANIM_STAND` and quietly skipped during ANIM_RUN, so a
running landing never got the standing-height correction.

Fix in `src/mech.c::mech_post_physics_anchor`:

- Run the anchor for `ANIM_STAND` *and* `ANIM_RUN`. JET/FALL/DEATH
  still skip naturally (grounded is false, or m->alive is false).
- Translate knees by the same `dy_pelvis` as the rest of the upper
  body (preserves the thigh constraint at rest length).
- Snap knees to mid-chain only in `ANIM_STAND`. In `ANIM_RUN` we let
  the stride animation drive knee X swing.

Verified against `b1_dense`, `m1_big_fall`, `m1_stuck_recovery`, and
`walk_right` — all show clean landings and correct stride pose.

(My initial diagnostic note pointed at "stretched legs / body pinned
high" in the fall script. That turned out to be a misread caused by
the shot-mode camera using `GetFrameTime()`, which is ~1 ms per draw
in shot mode and made the smooth-follow camera lag far behind a
fast-moving player. Fixed alongside: `Renderer.cam_dt_override`,
shot mode pins it to 1/60.)

### B2 — Jet recovers from B1 (no longer relevant)

With B1 fixed, the body recovers without needing to jet. Recovery
script kept around because the user-facing intuition ("if it ever
looks weird, jet to reset") is still worth verifying we don't break.

### B3 — Rifle and forearm rendered through thin solids (FIXED 2026-05-03)

Repro: `tests/shots/b3_arm_clip.shot`. Walking up to the col-55 wall
with the cursor on the far side puts the rifle aim direction into
the wall. Pre-fix, the rifle barrel and forearm bones drew straight
through the tiles even though both endpoint particles were outside
them. Cause was rendering, not physics: `draw_mech` in `src/render.c`
emitted a raw `DrawLineEx` from elbow→hand and from hand→muzzle with
no awareness of level geometry.

Fix: new `draw_bone_clamped(level, a, b, ...)` in `src/render.c`.
Uses the existing `level_ray_hits` DDA traversal; if the segment
hits a solid, the line stops at the hit point (with a 0.02 pull-back
so we don't draw on the tile edge). Applied to the rifle barrel,
both forearms, both back-arm forearms. Other body bones (torso,
shoulder plate, hip plate, thighs, shins, neck) stay raw because
particle-vs-tile collision keeps those from crossing in practice.
`draw_mech` now takes a `Level *` parameter.

### B4 — Body wedged against world ceiling under sustained jet (FIXED 2026-05-03)

Repro: `tests/shots/b4_ceiling.shot`. The level has no explicit
ceiling tiles; `level_tile_at` returns `TILE_SOLID` for `ty < 0` so
out-of-bounds is treated as solid for collision. Sustained jet rose
the player into that boundary, where collision pushed every particle
back down while the next tick's jet pushed them up again — the
constraint solver tangled the chain trying to keep bones rigid
against the wedge.

Fix in `src/mech.c::apply_jet_force`: taper the thrust as the head
particle approaches y=0. New constants `JET_CEILING_TAPER_BEGIN=64`
and `JET_CEILING_TAPER_END=24` (px below ceiling). At >64 px below,
full thrust; between 64 and 24, linearly scaled; above 24, zero
thrust. Player can still cap their altitude near the ceiling but
can't hammer into it. After release, gravity pulls them back into
the level cleanly.

### Not yet verified

- **Dummy ragdoll glitch on death.** The kill scripts couldn't
  deliver shots to the dummy because the col-55 wall blocks both
  travel and line-of-sight from the spawn approach (air control is
  98 px/s, not enough to clear a 32 px wall in the time a 5-tile-tall
  jet allows). Need either a script that gets on top of the wall, a
  wider jet window, or a fresh layout — separate task.

### B5 — "Long red rectangle" trailing the dummy on death (FIXED 2026-05-03)

Repro: `tests/shots/m1_dummy_kill.shot` (uses the new `spawn_at`
directive to park the player on the dummy platform, fires until the
dummy drops). Pre-fix, the dead dummy left a thick red bone-shaped
line stretched from the corpse to the right wall — sometimes the
dummy "shot off" cleanly, sometimes the line was huge.

Two compounding causes, both fixed:

1. **Double-scaled kill impulse in `weapons.c`.** `weapons_fire_hitscan`
   passed `dir * (recoil_impulse * 4)` (= `dir * 4.8`) into
   `mech_apply_damage` as the "direction" argument. `mech_kill` then
   multiplied that again by `90` (the kill impulse magnitude),
   producing a 432 px instant displacement on the killshot particle.
   When the killshot landed on a dismembered limb (e.g. L_ELBOW after
   a left-arm tear), that lone particle had no body mass to absorb
   the kick and flew clear across the level until it pinned against
   the world wall (~750 px from the chest in the dump). Fix: pass the
   unit `dir` through `mech_apply_damage` as it always was meant to
   be — `mech_kill`'s `impulse=90.0` arg is the only scale.
2. **Kill impulse applied to the killshot part rather than the
   pelvis.** Even at 90 px the issue persisted whenever the killshot
   was on a disconnected fragment. Now `mech_kill` applies the
   impulse to `PART_PELVIS`, which is always tied to the rest of the
   body via active constraints — the body ragdolls as a unit and
   detached limb pieces stay where they were when severed.
3. **Renderer drew bones whose distance constraints had been
   deactivated.** Even after fixing the physics, the shoulder-plate
   bone (L_SHO ↔ R_SHO) was still being drawn between the
   left-behind L_SHOULDER and the body's R_SHOULDER, producing the
   same long-line silhouette in a different orientation. Fix: new
   `bone_constraint_active(cp, a, b)` helper in `src/render.c`; every
   `draw_bone_chk` call skips bones whose backing distance constraint
   is inactive. Dismembered limbs simply disappear visually now
   (their particles still integrate; they're just not drawn).

### B6 — Jet tunnels through 1-tile-thick platforms (FIXED 2026-05-03)

Three compounding sources of tunneling, fixed in three steps:

1. **Inside-tile escape priority pushed UP** — when a particle ended
   up inside a 1-tile-thick platform from below, the original
   `open_up > open_left > open_right > open_down` priority pushed it
   straight through to the top with its upward velocity intact. Fix
   in `collide_map_one_pass` (`src/physics.c`): pick the exit side
   based on whichever side the particle's `prev` was on. The
   neighbour-priority falls back when `prev` was already inside the
   tile across multiple ticks.

2. **Constraint relaxation popped a particle past the tile** — even
   with the inside-tile fix, constraint solver can apply a >r-px
   correction in one iter, putting a particle past the tile entirely
   so the close-point collision check doesn't fire either. Fix:
   close-point branch now also checks `prev` and pushes the particle
   back to the side it came from when `prev` and current are on
   opposite sides of the tile.

3. **Pose drive teleported particles through the tile** (the
   user-reported "head ends up on the other side" case). The HEAD
   pose target is `(pelvis_x, pelvis_y - 52)` with strength 0.7.
   When the body jets up under a 1-tile-thick platform, the gap
   between current head and pose target can be 50 px; the kinematic
   translate (which moves both `pos` and `prev` together to avoid
   Verlet velocity injection) then teleports the head 35 px in one
   tick — straight through a 32-px platform. Trace at the spawn
   platform: HEAD jumped from y=996 to y=956 between T50 and T51, a
   40-px instant move with no collision check between.
   
   Fix: new `physics_translate_kinematic_swept` (in `src/physics.c`)
   that ray-casts from the current pos to the target and clamps the
   move r+½ px shy of the first solid tile crossing. Called from
   `apply_pose_to_particles` in `src/mech.c`. Pose still pulls the
   body toward standing-pose-relative-to-pelvis, but a strong pull
   can no longer skip a body part across a thin solid.

The integrate step also gained a swept-collision check for forces
(jet, gravity, kill impulse) that drive a particle through a tile in
one tick, in case forces alone build enough per-tick step to tunnel.

### B7 — Feet skitter across the ground while running (FIXED 2026-05-03)

The previous run pose swung each foot ±14 px around the hip with a
sinusoid at ~1.4 Hz. With the body moving at 280 px/s = 4.67 px/tick,
no point on that sin curve had body-frame velocity equal to
`-RUN_SPEED`, so a "planted" foot was always being dragged forward
through the world by `body_velocity + sin'(t)·swing` rather than
sitting still — visible as a constant skitter / micro-slide across
the ground.

Fix in `mech.c` ANIM_RUN pose: replace the sinusoid with an explicit
two-phase walk cycle. Each foot alternates between **stance** (foot
linearly moves backward in body-frame from `+stride/2` to `-stride/2`
over the first half of the cycle) and **swing** (foot lifts in a
sin-arc and moves forward to the next plant point over the second
half). Stride is fixed at 28 px and the cycle frequency is tied to
the run velocity such that one stride's worth of body motion exactly
covers the stance phase: `cycle_freq = RUN_SPEED / (2 · stride)` =
5 Hz at 280 px/s. The right foot is offset by 0.5 cycles so the two
are out of phase. Result: the planted foot's world velocity is
exactly zero during stance, and the body pivots over it confidently.

### Close-window delay (FIXED 2026-05-03)

Hitting the X on the window had a noticeable beat before the process
exited. The slow steps were `CloseAudioDevice` (miniaudio teardown),
`CloseWindow` (`glfwTerminate` / Wayland connection close), and the
arena destroys (~60 MB of `free`s). None of that work is necessary —
the kernel reclaims memory, the GL context, and the audio device on
process exit. `main.c` now does `log_shutdown(); _exit(0);` after the
loop and the close is instant. The shotmode path still does its own
shutdown sequence because shot mode wants its log messages flushed
before the process leaves.

### Verdict

All identified M1 close-out blockers fixed. The known remaining
rough edges — body launches very fast on death (90 px/tick momentum
on the pelvis carries it across most of the level before settling),
dismembered limb stays visually invisible — are tunable but no longer
"a line stuck across the screen." Up next is content/feel work.

---

## Visual debugging — shot mode

`src/shotmode.c` implements a `--shot <script>` mode that drives the
real `simulate()` + renderer with a tick-stamped input script and
writes PNGs at chosen ticks. It's how we (or a tool) inspect motion
and physics without filming.

```bash
make shot                                    # tests/shots/walk_right.shot
make shot SCRIPT=tests/shots/your_case.shot
```

Script grammar lives in `src/shotmode.h`. Output: `build/shots/*.png`,
plus a `<scriptname>.log` file in the same dir. The log is on by
default in shot mode (the `g_shot_mode` flag toggles `SHOT_LOG()` in
`src/log.h`); it captures one compact summary line per tick (anim,
grounded, pelvis pos/vel, fuel, hp, ammo) plus event lines for anim
transitions, grounded toggles, jet ceiling taper, post-physics anchor
recoveries (>1.5 px), pose-drive sweep clamps, inside-tile collision
escapes, hitscan fire/hit/miss, damage applied, dismemberment, and
kills. The log is paired 1:1 with the shot run so an LLM reviewing a
contact sheet can read exactly what happened during the run.
Fixed `dt = 1/60`, no wall-clock reads, RNG re-seedable from script —
re-runs are byte-identical.

---

## Recently fixed

### M4 — "black screen on client during match" (FIXED 2026-05-03)

Reported by the user after the first M4 ship. Both players reach the
lobby, both ready up, the round starts on the host, but the client
shows a black screen with no mech to follow.

Three compounding bugs:

1. **`lobby_decode_list` clobbered the just-decoded `mech_id`.** After
   `decode_one_slot` correctly populated `s->mech_id` from the wire,
   the next line stomped it back to -1 (a copy-paste from the
   pre-decode wipe loop). Net result: the client's lobby slots always
   showed `mech_id = -1`, even after the host shipped the mapping.
2. **`mech_id` was never on the LobbySlot wire format.** The ACCEPT
   handshake carries `slot_id`, but `world.mechs[]` indices are
   independent of slot ids when slots in the middle are unused. The
   client had no way to resolve "which snapshot mech is mine."
3. **`start_round` didn't broadcast the freshly-spawned mech_ids.**
   Even if the wire had carried mech_id, the host needed to ship the
   updated lobby table after `lobby_spawn_round_mechs` and *before*
   the snapshot stream began.

Fixes:

- Added `mech_id` to `LOBBY_SLOT_WIRE_BYTES` (now 40 bytes/slot).
- Removed the bogus reset in `lobby_decode_list`.
- `start_round` now ships `lobby_list` first, then `round_start`,
  both reliable on `NET_CH_LOBBY` (so they arrive in order).
- Per-frame check at the top of MODE_MATCH late-binds
  `world.local_mech_id` from `lobby.slots[local_slot].mech_id`
  once the corresponding mech actually shows up in the local
  snapshot.

The `tests/net/run.sh` script catches this regression — assertion
"client resolves local_mech_id → 1 (slot 1)" specifically watches for
the late-bind log line.

### M4 — "no Start button when both players ready" (FIXED 2026-05-03)

`server_handle_challenge_response` auto-armed a 60 s countdown when
the second peer joined. When both players hit Ready, the
`if (lobby_all_ready && !auto_start_active)` short-circuit skipped
re-arming, so the existing 60 s timer kept running. The "Start now"
button still showed but it read "Cancel start" with the long
countdown still ticking. Confusing.

Fix in `host_match_flow_step`: when all slots are ready, override the
remaining countdown to 3 s (only down, never up). Players who stayed
ready can now expect the round to start within 3 s of the last person
clicking Ready.

### M4 — server dropped ALL client inputs (FIXED 2026-05-03)

This was the actual root cause of the user's reported "extreme
client jitter" + "mouse pulls me around" + "client and server show
different states." Initial fixes (snapshot velocity sync, remote
mech lerp) helped but didn't address the root: **inputs from the
client were being silently dropped on the server**.

Diagnostic chain:
1. User reported jitter still present after the velocity-sync fix.
2. Built `tests/shots/net/2p_meet.{host,client}.shot` where both
   players walk toward each other to verify they actually meet.
3. Inspecting host log: client's mech (slot 1) stayed pinned at
   spawn x=2568 the entire match, while client's local prediction
   moved it to x=2446. Client and server completely disagreed.
4. Added one-shot logs to `net_client_send_input` and
   `server_pump_events` — client WAS sending NET_MSG_INPUT
   packets (~420 of them); host WAS receiving them (`type=3
   tag=0x07`); but `server_handle_input` never logged.
5. First line of `server_handle_input`:
   ```c
   if (p->state != NET_PEER_ACTIVE || p->mech_id < 0) return;
   ```
   `p->mech_id` was -1. Grepped for assignments — only set to -1
   in handshake; **never updated when the round starts**.

Why it broke specifically at M4: M2/M3 spawned the peer's mech at
challenge response (so `p->mech_id` was set immediately). M4
deferred mech spawn to round-start (mechs don't exist during
lobby), and `lobby_spawn_round_mechs` set `slot.mech_id` (the
LOBBY slot's mech_id) but nobody updated `p->mech_id` (the PEER
struct's mech_id).

Fix in `server_handle_input`: lazy-resolve `p->mech_id` from the
lobby slot the first time we see an input that arrives with
`p->mech_id == -1`.

```c
if (p->mech_id < 0) {
    int slot = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
    if (slot >= 0 && g->lobby.slots[slot].mech_id >= 0) {
        p->mech_id = g->lobby.slots[slot].mech_id;
    }
}
if (p->mech_id < 0) return;
```

Visible after the fix: `tests/shots/net/2p_meet`'s `meet_t580.png`
now shows both mechs side-by-side from each player's perspective —
the test that was specifically designed to make this bug visible.

### M4 — match.time_remaining frozen on client (FIXED 2026-05-03)

Spotted in the same `meet_t580.png` comparison: host's score
banner read "0s", client's read "8s" (= round-start value).
`match_state` is broadcast on round transitions only, never during
the active round, so the client's `time_remaining` was frozen at
whatever value `LOBBY_ROUND_START` carried. Fix: client locally
decays `match.time_remaining` each tick during MATCH_PHASE_ACTIVE
(same pattern we already used for `auto_start_remaining` and
`countdown_remaining`).

### M4 — extreme client-side jitter (FIXED 2026-05-03)

User report: "extremely jittery play on the client which cleared
up when I closed the host window." Two compounding bugs in
`snapshot_apply`:

1. **Velocity-sync was pelvis-only.** The translate set `prev = pos
   + delta` for all 16 particles (preserving each particle's old
   velocity), then `physics_set_velocity_*` overrode ONLY the
   pelvis velocity from the snapshot. Result: pelvis moving at v_p,
   neck/head/arms/legs all at their previous velocities — the
   constraint solver burned a few iterations every snapshot trying
   to reconcile the velocity mismatch, which the user saw as
   per-tick body shudder. Fix: set per-tick velocity on EVERY
   particle so the whole skeleton moves as a rigid unit.

2. **Remote-mech corrections were a hard snap.** Each snapshot the
   client snapped the remote pelvis to the server position. With
   the host walking at 280 px/s, that's a ~9 px jump every 33 ms
   (snapshot interval). Fix: smooth remote-mech corrections with a
   35 % lerp toward the snapshot position; over 3-4 snapshots we
   converge to the server state without the per-snapshot pop.
   Local mech still snaps fully so client-side reconcile (in
   reconcile.c) can replay unacked inputs from a known-truth
   anchor.

Diagnosed with `tests/shots/net/2p_jitter.{host,client}.shot`,
which extends the per-tick `SHOT_LOG` dump in `simulate.c` to
record EVERY mech's pelvis position (not just local), then
inspected the client log for oscillation patterns. Pre-fix log:
local pelvis bouncing 2528 ↔ 2568 (40-px swings); post-fix:
stable around 2567-2568 with one ≤22 px transient on a single
sample. See `tests/shots/net/run.sh 2p_jitter` to reproduce.

This is a partial implementation of the "render remote mechs
~100 ms in the past" interpolation called for in
[05-networking.md](documents/05-networking.md) §4. The lerp is
the cheap version; full snapshot-buffer interpolation lands
later (see TRADE_OFFS.md).

### M4 — visible-to-the-eye lobby UI bugs (FIXED 2026-05-03)

User noticed:

1. **Chat panel overlapping the loadout panel** — the chat panel
   was sized to full window width but the loadout panel sits at
   the right column from y=110 to y=506. Below `sh/2` they
   collided in the right-most ~340 px. Fix: explicit two-column
   layout (left: player list + chat; right: loadout + ready),
   chat constrained to `left_w` so it stops short of the
   loadout column.
2. **Match-start countdown numbers desync between host/client.**
   `NET_MSG_LOBBY_COUNTDOWN` was defined but never sent. The host
   now broadcasts on arm/cancel transitions and refreshes every
   0.5 s while active; the client decays its local `auto_start_
   remaining` each frame so the displayed number ticks smoothly
   between broadcasts. (See `host_broadcast_countdown_if_changed`
   in main.c, plus the per-frame decay in `MODE_LOBBY` for clients.)
3. **Mechs spawn on top of each other in FFA.** `MATCH_TEAM_FFA`
   aliases `MATCH_TEAM_RED` (both = 1), so `map_spawn_point`
   picked the red lanes (clustered at x=8..14) instead of the
   FFA lanes (spread across x=16..88). Fix: `map_spawn_point`
   now takes a `MatchModeId` arg and prefers the FFA lanes when
   `mode == MATCH_MODE_FFA` regardless of team.
4. **Lobby team chip read "Red" in FFA.** Same alias as above —
   `team_name(1)` returned "Red". Added `team_name_for_mode`
   that returns "FFA" when mode==FFA, plus a neutral-gray chip.
5. **Summary screen had HUD ghosting.** The translucent
   `(0,0,0,200)` panel let the HP bar / weapon labels bleed
   through. Bumped to `(4,6,10,240)` plus routed the summary
   overlay through the renderer's single Begin/EndDrawing pair
   so we don't double-present (which would have shown the world
   frame underneath the summary in alternating frames).

### M4 — top banner flicker (FIXED 2026-05-03)

The match score/timer banner was rendered in a *second*
`BeginDrawing/EndDrawing` pair after the world frame, producing a
present-swap-present sequence that alternated "world+HUD" and "stale
buffer + banner only" frames. Same root cause hit `MODE_SUMMARY`.

Fix: routed both overlays through the renderer's existing `overlay_cb`
callback so each rendered frame uses exactly one Begin/EndDrawing
pair. `OverlayCtx { Game *g; LobbyUIState *ui; }` packs both pieces
the callbacks need into the single `void *user`.

### Pre-M4 fixed bugs

The M1 implementation pass shipped, then we played it. These are the
real bugs we found and fixed, in chronological order — kept here so we
remember which classes of failure to look for next time.

### "My mech bounces around without me pressing any keys."

Three compounding bugs in the pose / drive / collision loop:

1. **Pose drive only updated `pos`, not `prev`.** Verlet reads
   `pos − prev` as velocity, so every pose lerp injected ghost velocity
   the next tick. Fixed by making pose drive a *kinematic translate*
   (update both `pos` and `prev` by the same delta) — see
   `physics_translate_kinematic()` and `apply_pose_to_particles()` in
   `src/mech.c`.
2. **Collision push didn't zero normal velocity.** Particles bounced off
   tile contacts because their post-push `prev` was on the other side of
   the tile. Fixed by adding `contact_position_only` (zero normal vel,
   keep tangential) for in-iteration contacts and `contact_with_velocity`
   (apply tangential friction) on the final iter only.
3. **Run force as `pos += force·dt`** accumulated velocity at 60× the
   intended rate. Fixed by using `physics_set_velocity_x()` to write the
   target velocity directly via `prev`.

### "I'm standing inside the platform. WASD doesn't move me."

- Run/jump/jet forces were applied to lower-body particles only. The
  upper body had no horizontal force, so the constraint solver fought
  the legs. Fixed by applying `apply_run_velocity`, `apply_jump`, and
  `apply_jet_force` to all 16 particles.
- Spawn was at the exact tile boundary. Added `foot_clearance = 4`
  (particle radius) when computing spawn Y so feet sit *on* the floor,
  not inside it.

### "Start/stop feels sluggish, mechs sink into the floor, dummy is folded in half."

- Single-pass constraint then single-pass collide didn't propagate. A
  foot held by the floor couldn't lift the body in the same tick — the
  pelvis sagged a little each frame instead. Fixed by interleaving
  constraint + collide inside one relaxation loop
  (`physics_constrain_and_collide`), 12 iterations.
- Particles inside a tile ping-ponged between adjacent solid tiles.
  Fixed with neighbour-aware exit (`collide_map_one_pass` checks
  up/left/right/down in priority order; pushes out toward the empty
  side).
- Active braking when grounded with no input — `apply_run_velocity`
  zeros the body's X velocity if there's no run input.

### Dummy's leg buckled to horizontal

This was the long one. The dummy at the far edge of the map was
mysteriously slumping over even though the player wasn't.

- We tried angle constraints. They had a modulo bug at the π boundary
  that injected huge corrections. Fixed the solver (use `acos(dot)`
  instead of an `atan2` difference) — but angle constraints **can't**
  prevent a leg from rotating to horizontal: the interior angle stays
  π whether the leg is upright or lying down. Removed.
- The pose target for `L_HAND` was far from the L_SHOULDER pose target.
  Pose was fighting the upper-arm + forearm chain on every tick. Fixed
  by dropping the L_HAND pose target entirely (the left hand dangles —
  see [TRADE_OFFS.md](TRADE_OFFS.md), no IK).
- The dummy's aim flipped between left and right as its chest sagged
  past the aim point. Fixed by making dummies skip the arm pose
  entirely (`if (!m->is_dummy)` around the aim-driven arm logic).
- The pelvis Y pose target was being read from the (already sagging)
  live pelvis. Foot-chain pose targets were derived from that, so they
  ended up under the floor; collision pinned them; the chain collapsed.
  Fixed by deriving the pelvis pose target from foot Y minus chain
  length when grounded — the body's *target* is always a standing one
  even if the live pelvis is briefly elsewhere.
- Even with that, soft pose strength wasn't enough; gravity sag still
  accumulated faster than the solver could correct it. Hardening the
  pose strength pumped velocity through the constraint solver (the
  classic "Verlet position-only correction → injected velocity" trap).
  Fixed by adding `mech_post_physics_anchor()`: after physics, if
  grounded and in `ANIM_STAND`, lift pelvis + upper body + knees to
  standing positions kinematically *and* zero their Y-velocity
  (`prev_y = pos_y`). Skipped during run/jump/jet/death so the body
  responds naturally to those.

### Pinned feet (`inv_mass = 0`) experiment

We tried pinning the feet during stand to anchor the body. That made it
explode upward during run because all the constraint corrections
landed on the only free end (the pelvis), creating a huge velocity
injection. Reverted; the post-physics anchor approach is what shipped.

---

## Known issues

These are real and reproducible. They aren't blockers for the current
milestone, but they're filed for future attention. Each links to its
longer-form entry in [TRADE_OFFS.md](TRADE_OFFS.md).

- **Left arm dangles** — No inverse kinematics yet. The left hand has
  no pose target; the upper-arm + forearm chain hangs from the
  shoulder. Looks weird with a two-handed rifle. *Resolution path:* M5
  P11 (two-handed foregrip pose for two-handed weapons; one-handed
  weapons still leave L_HAND dangling per the M5-introduced trade-off).
- **Knee snap during anchor** — `mech_post_physics_anchor` snaps knees
  to mid-thigh+shin. It's invisible at idle but would break a crouch
  cycle, which is why the anchor only runs in `ANIM_STAND` / `ANIM_RUN`
  on flat ground (P02 added the slope-gating).

---

## Tunables (current values)

These are the numbers driving the feel. Documented here so we can A/B
them against playtest reactions. (Per-weapon stats are in the
`g_weapons[]` table in `src/weapons.c` — too many to mirror here.)

| Source                    | Constant                        | Value    |
|---------------------------|---------------------------------|----------|
| `src/physics.h`           | `PHYSICS_GRAVITY_PXS2`          | 1200     |
| `src/physics.h`           | `PHYSICS_RKV` (Verlet damping)  | 0.99     |
| `src/physics.h`           | `PHYSICS_CONSTRAINT_ITERATIONS` | 12       |
| `src/mech.c`              | `RUN_SPEED_PXS`                 | 280      |
| `src/mech.c`              | `JUMP_IMPULSE_PXS`              | 320      |
| `src/mech.c`              | `JET_THRUST_PXS2`               | 2200     |
| `src/mech.c`              | `JET_DRAIN_PER_SEC`             | 0.60     |
| `src/mech.c`              | `AIR_CONTROL`                   | 0.35     |
| `src/mech.c`              | `BINK_DECAY_PER_SEC`            | 1.8      |
| `src/mech.c`              | `BINK_MAX`                      | 0.35 (~20°) |
| `src/mech.c`              | `SCOUT_DASH_PXS`                | 720      |
| `src/mech.c`              | `ENGINEER_HEAL`                 | 50       |
| `src/mech.c`              | `ENGINEER_COOLDOWN`             | 30 s     |
| Chassis HP (Scout/Trp/Heavy/Sniper/Eng) | `health_max`          | 100/150/220/130/140 |
| Armor capacity (None/Light/Heavy/Reactive) | `hp`               | 0/30/75/30 |
| Armor absorb_ratio        | (Light/Heavy/Reactive)          | 0.40/0.60/0.40 |
| `src/weapons.c` (Pulse)   | damage / cycle / reload         | 18 / 110ms / 1.5s |
| `src/weapons.c` (Pulse)   | mag / range                     | 30 / 2400 |
| `src/weapons.c` (Mass Driver) | direct + AOE_dmg / radius   | 220 + 130 / 160 |
| `src/weapons.c` (Frag)    | AOE_dmg / radius / fuse         | 80 / 140 / 1.5s |
| `src/weapons.c` (Rail)    | damage / charge / cycle         | 95 / 400ms / 1200ms |
| Limb HP (arm/leg/head)    | `hp_*_max`                      | 80 / 80 / 50 |

The simulation runs at **60 Hz** at M1 (not 120 Hz as
[documents/03-physics-and-mechs.md](documents/03-physics-and-mechs.md)
specifies). See "60 Hz vs 120 Hz" in [TRADE_OFFS.md](TRADE_OFFS.md).

---

## Playtest log

Running record of what the user (the only playtester right now) said,
what we did about it, and whether it was fixed. Most-recent first.

| Date | Report | Outcome |
|------|--------|---------|
| 2026-05-03 | Jet feels different in fullscreen vs windowed | Diagnosed as vsync/render-rate mismatch. Fix deferred to render-side interpolation. |
| 2026-05-03 | "Standing inside the platform, body folded, WASD doesn't move me" | Foot clearance + interleaved constraint/collide + full-body forces. **Fixed.** |
| 2026-05-03 | "Bounces around without input" | Kinematic pose translate + contact normal-vel zeroing + run force via prev. **Fixed.** |
| 2026-05-03 | "Dummy folded in half across the map" | Removed angle constraints, dropped L_HAND pose, derived pelvis target from feet, post-physics anchor with vel-zero. **Fixed.** |

When new playtest sessions happen, append rows here.
