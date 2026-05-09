# CURRENT_STATE — Where the build actually is

A living snapshot of debugging and playtesting. Updated as the build
moves. The design documents in [documents/](documents/) describe the
*intent*; this file describes the *current behavior* of the code that's
sitting on disk right now.

Last updated: **2026-05-05** (M4 lobby & matches in; M5 P01 — `.lvl` format + loader/saver, P02 — polygon collision + slope physics, P03 — render-side accumulator + interp alpha + reconcile smoothing + two-snapshot remote interp + `is_dummy` bit, P04 — standalone level editor at `tools/editor/` + `--test-play` flag, P05 — pickup runtime + powerups + Engineer deployable + Burst SMG cadence + practice dummy, P06 — grappling hook with `CSTR_FIXED_ANCHOR` + contracting distance constraint + state-bit-gated wire suffix, P07 — CTF mode with flag entities + capture rule + carrier penalties + 26-byte `NET_MSG_FLAG_STATE` event protocol; post-P07 — lobby UX pass, auto-balance ordering fix, editor → game CTF round-trip via F5, snapshot pos quant 8× → 4× to fit Crossfire's 4480 px width; P08 — map sharing across the network: `MapDescriptor` in `INITIAL_STATE`, `NET_MSG_MAP_REQUEST`/`MAP_CHUNK`/`MAP_READY`/`MAP_DESCRIPTOR`, `src/map_cache.{c,h}` + `src/map_download.{c,h}`, content-addressed cache at `<XDG/AppData>/soldut/maps/<crc>.lvl` with 64 MB LRU, lobby UI download progress, host gate on per-peer MAP_READY; P08b — runtime `MapRegistry` with up-to-32 entries; scans `assets/maps/*.lvl` at `game_init` so user-authored maps surface in the host's lobby cycle without overwriting a builtin slot; P09 — `BTN_FIRE_SECONDARY` (RMB) one-shot of inactive slot, host kick/ban modal in the lobby player list, `bans.txt` persistence, three-card map vote picker on the summary screen, title-screen Controls modal listing keybinds; P10 — per-chassis bone-length distinctness pass + per-chassis posture quirks in `build_pose` + new `src/mech_sprites.{c,h}` module + sprite-or-capsule dispatch in `draw_mech` (sprite path renders 17-entry `g_render_parts` z-order; capsule fallback when `assets/sprites/<chassis>.png` is missing) + `extra_chassis` shotmode directive + `tests/shots/m5_chassis_distinctness.shot`).

---

## Milestones

| Milestone | Status                                  |
|-----------|-----------------------------------------|
| **M0**    | Done — see [README.md](README.md).      |
| **M1**    | Playable end-to-end. B1, B3, B4, B5, B6, B7 fixed 2026-05-03. Close-delay fixed same day. |
| **M2**    | Foundation lands 2026-05-03. Host/client handshake works locally; per-tick input ship + 30 Hz snapshot broadcast + client-side prediction & replay + per-mech bone history for hitscan lag compensation are wired. LAN-only, full snapshots, no mid-tick interpolation of remote mechs (see TRADE_OFFS.md). Two-laptop bake test still pending. |
| **M3**    | Combat depth in 2026-05-03. All 5 chassis (Trooper / Scout / Heavy / Sniper / Engineer) with passives. All 8 primaries (Pulse Rifle, Plasma SMG, Riot Cannon, Rail Cannon, Auto-Cannon, Mass Driver, Plasma Cannon, Microgun) and 6 secondaries (Sidearm, Burst SMG, Frag Grenades, Micro-Rockets, Combat Knife, Grappling Hook). Projectile pool with bone + tile collision. Explosions: damage falloff, line-of-sight check, impulse to ragdolls. Per-limb HP and dismemberment of all 5 limbs. Recoil + bink + self-bink fully wired. Friendly-fire toggle (`--ff` server flag). Kill feed with HEADSHOT/GIB/OVERKILL/RAGDOLL/SUICIDE flags. Loadout via CLI flags (`--chassis`, `--primary`, `--secondary`, `--armor`, `--jetpack`). Snapshot wire format widened to carry chassis/armor/jet/secondary; protocol id bumped to `S0LE`. |
| **M4**    | Lobby & matches in 2026-05-03. Game flow is now title → browser → lobby → countdown → match → summary → next lobby. New modules: `match.{h,c}`, `lobby.{h,c}`, `lobby_ui.{h,c}`, `ui.{h,c}` (small immediate-mode raylib UI helpers, scale-aware for 4K), `config.{h,c}` (`soldut.cfg` key=value parser), `maps.{h,c}` (Foundry / Slipstream / Reactor — three code-built maps for the rotation; `.lvl` loader is M5). LOBBY-channel messages (player list with `mech_id`, slot delta, loadout, ready, team change, chat, vote, kick/ban, countdown, round start/end, match state). Server config file: port, max_players, mode, score_limit, time_limit, friendly_fire, auto_start_seconds, map_rotation, mode_rotation. Single-player flow auto-hosts an offline server and arms a 1s countdown. Protocol id bumped `S0LE` → `S0LF`. Network test scaffold under `tests/net/` runs the host/client end-to-end via real ENet loopback and asserts on log-line milestones. |
| **M5**    | In progress. **P01–P10** in. P02: per-particle contact normals (Q1.7 SoA fields + `PARTICLE_FLAG_CEILING`); polygon broadphase grid built at level load (`level_build_poly_broadphase`); `physics_constrain_and_collide` interleaves tile + polygon collision per relaxation iter (closest-point-on-triangle, push-out via pre-baked edge normals); `level_ray_hits` tests segments against polygons too; slope-tangent run velocity, slope-aware friction (`0.99 - 0.07*|ny|`, ICE→0.998), angled-ceiling jet redirection, slope-aware post-physics anchor (skips when `ny_avg > -0.92`); WIND/ZERO_G ambient zones; environmental damage tick for DEADLY tiles+polys+ACID zones. Renderer draws polygons (P02 stopgap, replaced by sprite art at P13). P03: per-particle `render_prev_x/_y` snapshot at the top of each `simulate_step`; renderer lerps `pos` ↔ `render_prev` by `alpha = accum/TICK_DT` (also threaded through projectile + FX draw); reconcile `visual_offset` is now read by `renderer_draw_frame` and applied additively to local-mech draws (decays over ~6 frames so server snaps don't read as glitches); per-mech remote snapshot ring (`remote_snap_ring[8]`) + `snapshot_interp_remotes` lerps remote mechs at `client_render_time_ms - 100ms` between bracketing entries (clamped to nearest if only one entry, snap+clear on >200 px corrections); `state_bits` widened u8→u16, `SNAP_STATE_IS_DUMMY` rides bit 11 so client dummies don't drive arm-aim; protocol id bumped `S0LF` → `S0LG`. P05: new `src/pickup.{c,h}` with PickupPool (capacity 64) on World; `pickup_init_round` populates from `level->pickups` and spawns practice-dummy mechs on the authoritative side; `pickup_step` (server-only, per tick) does 24 px touch detection + cooldown rollover + transient lifetime expiry; per-kind apply rules with full-state-rejects-grab guards; powerup timers `powerup_berserk/invis/godmode_remaining` on Mech with `SNAP_STATE_BERSERK/INVIS/GODMODE` bits in the snapshot (clients mirror to sentinel timers); berserk doubles outgoing damage and godmode zeroes incoming damage in `mech_apply_damage`; render alpha-mods invis-active mechs (0.2 / 0.5 local). Engineer's `BTN_USE` now spawns a TRANSIENT `PICKUP_REPAIR_PACK` at the engineer's feet (10 s lifetime, 30 s cooldown) instead of self-healing — allies + the engineer can grab it. Burst SMG fires round 1 on press tick + queues `burst_pending_rounds` to fire at `burst_interval_sec` cadence in `mech_step_drive`. New wire message `NET_MSG_PICKUP_STATE = 15` (20 bytes — full spawner data so transients propagate). New world ring `pickupfeed[64]` drained per tick by `broadcast_new_pickups` in main.c. New regression test `tests/pickup_test.c` (`make test-pickups`). P06: new per-Mech `Grapple` struct + `PROJ_GRAPPLE_HEAD` projectile + `CSTR_FIXED_ANCHOR` constraint (Constraint grows by `Vec2 fixed_pos`); `mech_try_fire`'s WFIRE_GRAPPLE branch spawns a 1200 px/s head from R_HAND (was `NOT YET IMPLEMENTED`); on hit `projectile_step` sets firer state to ATTACHED + calls `mech_grapple_attach` which appends a constraint to the global pool (CSTR_FIXED_ANCHOR for tile, CSTR_DISTANCE_LIMIT min=0/max=L for bone — both one-sided so the rope is slack when shorter than rest and taut when stretched). `mech_step_drive` keeps rest length fixed at hit-time distance (no auto-contract — the Tarzan/pendulum feel) and emits a per-second `grapple_swing` SHOT_LOG; releases on BTN_USE edge or anchor-mech death; `mech_kill` releases on firer death; `mech_grapple_release` flips `active=0` (slot leak bounded). BTN_FIRE while ATTACHED chain-releases + fires a new head (cooldown still gates back-to-back fires at 1.20 s). Snapshot: `SNAP_STATE_GRAPPLING` bit 12 gates an optional 8-byte trailing suffix on `EntitySnapshot` (state, anchor_mech, anchor_part, anchor_x_q, anchor_y_q) so idle bandwidth stays flat. Render: new `draw_grapple_rope` in `render.c` draws a 1.5-px gold line hand → live head (FLYING) or hand → anchor (ATTACHED, tile or bone particle). Two shot tests: `tests/shots/m5_grapple.shot` (basic fire/attach/swing/release) + `tests/shots/m5_grapple_swing.shot` (pendulum + chain re-fire while attached). Protocol id stays `S0LG`. P07: new `src/ctf.{c,h}` module — server-authoritative flag entities (`Flag flags[2]` + `flag_count` on World, populated by `ctf_init_round` from `level.flags` LvlFlag records; flags[0]=RED, flags[1]=BLUE convention). `ctf_step` per tick: 36 px touch detection, 30 s auto-return, both-flags-home capture rule (carrier touching own HOME flag while carrying enemy → +5 team, +1 slot, captured flag returns home). `ctf_drop_on_death` from `mech_kill` drops at pelvis pos with `FLAG_AUTO_RETURN_TICKS` pending. Carrier penalties: `apply_jet_force` halves thrust when `ctf_is_carrier`; `mech_try_fire` rejects when `active_slot == 1 && ctf_is_carrier`. New world fields: `flag_state_dirty` (mutation hint, broadcast site is `main.c::broadcast_flag_state_if_dirty`) + `match_mode_cached` (so `mech_kill` can branch without seeing Game). New wire message `NET_MSG_FLAG_STATE = 16` (variable: 1 byte tag + 1 byte flag_count + 12 bytes per flag — team / status / carrier_mech / pos_q / return_in_ticks; max 26 bytes for a both-flags broadcast). INITIAL_STATE appends optional flag-state suffix for joining clients. CTF score limit defaults to `FLAG_CAPTURE_DEFAULT = 5` when config has the FFA-default `>=25`. Mode-mask validation in `start_round`: if rotation lands on CTF and the picked map's META.mode_mask doesn't allow CTF (or has no Red+Blue flag pair), demote to TDM and log warning (the build-then-validate path takes one extra map_build per CTF round). Render: `draw_flags` after mech body — vertical staff + triangular pennant team-colored, sin-driven wobble while CARRIED, outline halo for DROPPED. HUD: `draw_flag_pips` (Red top-left + Blue top-right corner pips, alpha-pulse for CARRIED, outline-only for DROPPED) + `draw_flag_compass` (off-screen flag → triangular arrow at the nearest screen edge pointing at the flag, team-colored). New regression test `tests/ctf_test.c` (52 assertions, `make test-ctf`): init_round, flag_position, enemy pickup, friendly no-pickup, carrier-dies drop, friendly return, auto-return on timer, capture, ctf_is_carrier, no-capture-without-carry. Protocol id stays `S0LG` (FLAG_STATE event uses a free message id 16, no entity-snapshot widening). P08: new modules `src/map_cache.{c,h}` + `src/map_download.{c,h}`; 32-byte `MapDescriptor` (crc32 + size_bytes + short_name[24] padded to 36 wire bytes via reserved[3]) appended to every `INITIAL_STATE` body so connecting clients learn the host's current map; four new wire messages on `NET_CH_LOBBY` (`NET_MSG_MAP_REQUEST=40`/`MAP_CHUNK=41`/`MAP_READY=42`/`MAP_DESCRIPTOR=43`); chunk size 1180 bytes (matches the 1200-byte ENet MTU after the 16-byte chunk header); content-addressed cache at `<XDG_DATA_HOME or platform default>/soldut/maps/<crc32_hex>.lvl` with 64 MB LRU eviction (atomic write via `<crc>.lvl.tmp` + rename); resolve order on the client = `assets/maps/<short>.lvl` with matching CRC, then `<cache>/<crc>.lvl`, else download; `level_compute_buffer_crc` exposed from level_io.c so `client_finalize_map_download` can verify reassembled bytes against descriptor; `maps_refresh_serve_info` recomputes the host's serve descriptor + serve_path after every map_build (bootstrap_host, start_round, lobby UI mode/map cycle); host's auto-start countdown holds at ≥1.5 s while any peer's `map_ready_crc` doesn't match the current map crc (slow downloaders still join the round; the gate just defers the fire so they don't miss spawn); 30 s stall watchdog on the client cancels + disconnects if no chunks arrive; lobby UI shows `DOWNLOADING MAP NN%` progress strip when `g->map_download.active`; mid-lobby host map changes broadcast `NET_MSG_MAP_DESCRIPTOR` so clients re-resolve; protocol id stays `S0LG` (additive). New regression tests: `tests/map_chunk_test.c` (21 assertions, `make test-map-chunks`) covering chunk reassembly + duplicate detection + OOB rejection + bit-flip CRC fail; `tests/synth_map.c` writes a custom .lvl on demand; `tests/net/run_map_share.sh` (15 assertions) end-to-end host streams + client downloads + caches + plays. Version string `0.0.7-m5p08`. P08b: runtime `MapRegistry` (32-slot cap) replaces the static 4-entry `MapId` enum. `game_init` calls `map_registry_init` after `map_cache_init` (and before `config_load`, so `map_id_from_name` resolves `map_rotation=my_arena` against on-disk `.lvl` files). Scan reads each `assets/maps/*.lvl`'s 64-byte header + 32-byte META lump cheaply (no `level_load`) and pulls display name from STRT (falling back to titlecased filename stem like `my_arena` → `My Arena`). Builtin overrides are by short_name match — saving `assets/maps/foundry.lvl` from the editor stamps the builtin's slot with disk CRC + size + on-file mode_mask without losing the reserved index. Custom-map IDs (>= `MAP_BUILTIN_COUNT`) get a separate hard-fallback path in `map_build` (LOG_E + Foundry's code-built) instead of falling through to `build_fallback`'s switch default. Lobby UI cycle iterates `g_map_registry.count` (lobby_ui.c three sites). Client display falls back to `g->pending_map.short_name` when `match.map_id` is outside its local registry (host has a custom map the client doesn't, before INITIAL_STATE arrives). New regression test `tests/map_registry_test.c` (`make test-map-registry`) and end-to-end `tests/shots/net/run_meet_named.sh` (`make test-meet-named`, 18 assertions) — proves the editor → host → client flow works on a non-reserved map name (`my_arena`). Resolves the "Custom map names not in lobby rotation (P08 follow-up)" trade-off. P09: `BTN_FIRE_SECONDARY = 1u << 12` in `src/input.h`, RMB sampled in `src/platform.c`; new static `fire_other_slot_one_shot` in `src/mech.c` swap-dispatches the inactive slot via the existing `weapons_fire_*` helpers (HITSCAN / PROJECTILE / SPREAD / THROW / BURST / MELEE / GRAPPLE) and restores the active aliases — charge weapons rejected, flag carrier secondary still fully disabled, shared `fire_cooldown` gates LMB+RMB so neither path doubles DPS; mech_try_fire calls it on `BTN_FIRE_SECONDARY` edge ABOVE the active-slot guard so the inactive primary remains usable when the active secondary is gated. New `lobby_load_bans` / `lobby_save_bans` in `src/lobby.{c,h}` — flat one-line-per-ban `bans.txt` next to the binary (`<addr_hex> <name>`), loaded in `bootstrap_host` and auto-saved at the end of `lobby_ban_addr` (path captured on `LobbyState`). New `net_server_kick_or_ban_slot` in `src/net.{c,h}` — host-side direct call (no wire round-trip); the existing `server_handle_lobby_kick_or_ban` calls it after host validation. Lobby UI: `[Kick] [Ban]` buttons in `player_row` for non-host slots when `net.role == NET_ROLE_SERVER`; click stages a confirmation modal in `lobby_screen_run` with explicit Cancel/Confirm. Three-card map vote picker on the summary screen: `host_start_map_vote` picks 3 distinct mode-compatible maps from `g_map_registry` (Fisher-Yates shuffle), arms `lobby_vote_start` with 80% of the summary timer, broadcasts `NET_MSG_LOBBY_VOTE_STATE`; cards render in `summary_screen_run` with placeholder gray thumbnails (real art at P13/P16), live tally, and per-card Vote button; client routes through new `apply_map_vote` (wire for clients, in-process for host). `begin_next_lobby` reads `lobby_vote_winner` and overrides `g->match.map_id` before the lobby state reset. Title screen gains a small Controls button bottom-right that opens a 13-row keybinds modal — sourced from `src/platform.c` (the canonical source of truth) so it lists the actual game keys (`X` for prone, `F` for melee), with Right Mouse → "Secondary fire — inactive slot, NEW". `lobby_tick` is now also called during `MATCH_PHASE_SUMMARY` so the vote countdown decays. New shotmode button name `fire_secondary` (BTN_FIRE_SECONDARY); new shot test `tests/shots/m5_secondary_fire.shot` proves a Trooper LMB-fires Pulse Rifle, RMB-throws a Frag Grenade without flipping active_slot, then LMB-fires Pulse Rifle again. Resolves three M4-era trade-offs (deleted from `TRADE_OFFS.md`): "Map vote picker UI is partial", "Kick / ban UI not exposed", "`bans.txt` not persisted". P10: per-chassis bone-length distinctness pass in `g_chassis[]` (`src/mech.c`) — Heavy bumped (arm 16→17, torso 34→38, neck 14→16), Scout shrunk (-16% all over), Sniper anisotropic (forearm 17→19, shin 18→21, neck 14→16), Engineer compact (forearm 16→14, thigh 18→16, torso 30→32). Bone changes ripple through `mech_create_loadout`'s `DIST(...)` rest lengths and every `ch->bone_*` read in `build_pose`. Per-chassis posture quirks in `build_pose`: Scout forward chest lean, Heavy chest strength 0.7→0.85, Sniper head offset down+forward, Engineer skips right-arm aim drive when `active_slot == 1`. New `src/mech_sprites.{c,h}` — `MechSpriteSet g_chassis_sprites[CHASSIS_COUNT]` with 22-entry `MechSpritePart` table per chassis (sub-rect + pivot + draw_w/h), populated from a shared Trooper-sized placeholder layout per `documents/m5/12-rigging-and-damage.md` §"Sprite anatomy". `mech_sprites_load_all()` walks `assets/sprites/<chassis>.png` for each chassis; missing files leave `atlas.id == 0` so the renderer falls back per-chassis. New `draw_mech_sprites` in `src/render.c` walks a 17-entry `g_render_parts[]` z-order table (back limbs → centerline body → front limbs); when `m->facing_left`, `swap_part_lr`/`swap_sprite_lr` flip L↔R per entry. Sprite rotation `atan2(b - a) * RAD2DEG - 90.0f`. Old capsule path preserved as `draw_mech_capsules` (no-asset/dev fallback); held-weapon line factored into shared `draw_held_weapon_line`. `_Static_assert` locks `MECH_RENDER_PART_COUNT == 17`. Atlas load wired in `main.c` (after `platform_init`) and in `shotmode.c` (with `unload` before each `platform_shutdown`). New shotmode directive `extra_chassis <name> <x> <y>` (cap 6) spawns side-by-side dummies for comparison shots; new test `tests/shots/m5_chassis_distinctness.shot` puts all 5 chassis in one frame for visual verification. **Pending**: P11–P18 (held-weapon sprite art, damage feedback, parallax / HUD final / TTF / halftone, audio, asset generation, authored maps). |

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

- **P05 — pickup runtime + Engineer deployable + Burst SMG cadence +
  practice dummy** (2026-05-04).
  - New module `src/pickup.{c,h}`. Public API: `pickup_init_round`,
    `pickup_spawn_transient`, `pickup_step`, `pickup_default_respawn_ms`,
    `pickup_kind_color`. Pool `World.pickups` (capacity 64) holds both
    level-defined spawners and engineer-deployed transients (the
    `PICKUP_FLAG_TRANSIENT` bit distinguishes them). Per-kind apply
    rules with the "full-state rejects grab" guard so a player at full
    HP doesn't waste a HEALTH pack by walking over it.
  - **Powerups**: three timer fields on Mech (`powerup_berserk_remaining`,
    `powerup_invis_remaining`, `powerup_godmode_remaining`). Server
    ticks them in `mech_step_drive`; clients mirror via the new
    `SNAP_STATE_BERSERK = 1<<8`, `SNAP_STATE_INVIS = 1<<9`,
    `SNAP_STATE_GODMODE = 1<<10` bits in `EntitySnapshot.state_bits`
    (already widened to u16 at P03; no new protocol bump). Berserk
    doubles outgoing damage, godmode zeroes incoming damage — both
    centralized in `mech_apply_damage`. Invisibility alpha-mods the
    mech's drawn color (alpha 51 for remote viewers, 128 for the local
    mech so the player can still see themselves).
  - **Engineer ability rewrite**: `BTN_USE` on the Engineer chassis
    spawns a TRANSIENT `PICKUP_REPAIR_PACK` at the engineer's pelvis
    via `pickup_spawn_transient` (10 s lifetime, 30 s cooldown).
    Allies + the engineer himself walking onto it consume it for
    +50 HP. Replaces M3's instant self-heal — the M3 trade-off entry
    is gone.
  - **Burst SMG cadence**: new fields `burst_pending_rounds` (u8) +
    `burst_pending_timer` (float) on Mech. `mech_try_fire` spawns
    round 1 on the press tick and queues the remaining
    `burst_rounds-1`. Top of `mech_step_drive` ticks the timer and
    spawns the next round when it hits zero, repeating at
    `burst_interval_sec` cadence (70 ms for the Burst SMG). Server-side
    only — clients receive the per-round NET_MSG_FIRE_EVENT broadcasts
    and don't predict the trailing rounds. The M3 "all rounds on one
    tick" trade-off entry is gone.
  - **Practice dummy**: `pickup_init_round` recognizes the
    `PICKUP_PRACTICE_DUMMY` kind and spawns a dummy mech at the
    spawner's position (server-only — clients receive it via the
    snapshot stream with `SNAP_STATE_IS_DUMMY` set). The spawner is
    immediately marked `state=COOLDOWN`, `available_at_tick=UINT64_MAX`
    so it never appears as a pickup. Maps that include a
    `PICKUP_PRACTICE_DUMMY` spawner work for single-player testing.
    The M4 "no practice dummy" trade-off entry is gone.
  - **Wire protocol**: new message `NET_MSG_PICKUP_STATE = 15` on
    `NET_CH_EVENT` (reliable, ordered). 20 bytes per event:
    `(spawner_id, state, reserved, available_at_tick, pos_q, kind,
    variant, flags)`. Spec doc 04-pickups.md called for 12 bytes;
    M5 P05 widened to 20 to support transient-spawner replication
    (engineer repair packs need pos/kind on the wire because clients
    haven't seen them at level-load time). Bandwidth: ~53 B/s
    aggregate at 16 players, well under budget.
  - **Per-tick flow** in `simulate_step`: `pickup_step(w, dt)` runs
    after `mech_apply_environmental_damage`. State changes get queued
    on `World.pickupfeed[64]` (monotonic counter ring, same shape as
    HitFeed/FireFeed); main.c's `broadcast_new_pickups` drains it
    each tick alongside `broadcast_new_hits`/`broadcast_new_fires`
    and ships a `NET_MSG_PICKUP_STATE` per entry. `pickup_init_round`
    runs in both `start_round` (host) and `client_handle_round_start`
    (clients), so both sides populate identically from the level data.
  - **Renderer**: `draw_pickups(pool, now)` placeholder — colored
    bobbing circles per `pickup_kind_color` lookup. Replaced by the
    sprite atlas at P13; PRACTICE_DUMMY entries are skipped (the dummy
    is a real mech, not a pickup).
  - **Regression test** `tests/pickup_test.c` (32 assertions,
    `make test-pickups`): HEALTH grab refills HP + transitions to
    COOLDOWN; HEALTH grab is REJECTED at full HP; berserk doubles
    outgoing damage; godmode caps a 9999-dmg headshot at 0 HP loss;
    invis sentinel timer mirrors the snapshot bit; transient lifetime
    expiry sets `available_at_tick = UINT64_MAX`; Burst SMG spawns
    one projectile on the press tick and two more across subsequent
    ticks at the 70 ms cadence (3 total).
  - Verification: `make` clean; `make test-physics` ok;
    `make test-level-io` 25/25; `make test-pickups` 32/32;
    `make test-spawn` ok; `tests/net/run.sh` 13/13;
    `tests/net/run_3p.sh` 10/10. Protocol id stays `S0LG` (no wire
    rev bump beyond P03; powerup bits ride existing reserved bits).

- **P06 — Grappling hook** (2026-05-04).
  - **Per-Mech state**. `world.h` adds `GrappleState` enum
    (`IDLE / FLYING / ATTACHED`) and a `Grapple` struct
    (`state, anchor_mech, anchor_part, anchor_pos, rest_length,
    constraint_idx`); `Mech` gains `Grapple grapple`. Initialised
    in `mech_create_loadout` with `constraint_idx = -1,
    anchor_mech = -1` so a stale 0 isn't read as "the first
    constraint slot is mine".
  - **Constraint kind**. New `CSTR_FIXED_ANCHOR` in `ConstraintKind`;
    `Constraint` grows by `Vec2 fixed_pos` (8 bytes — pool grows by
    16 KB at 2048 cap, trivial). `physics.c::solve_fixed_anchor`
    pulls particle `a` toward `c->fixed_pos` by the contracting
    `c->rest`; the anchor end has effective inv_mass = 0 so the
    particle takes the full correction. Tile anchors use this kind;
    bone anchors reuse `CSTR_DISTANCE` between firer pelvis and the
    target's bone particle (symmetric; mass-scale asymmetry pulls
    a Heavy toward a Scout much less than vice-versa).
  - **Projectile kind**. `PROJ_GRAPPLE_HEAD` added to
    `ProjectileKind`; `projectile_step` special-cases it before the
    damage path. On tile/bone hit: lands at the clamped point
    (clamped 4 px back along flight direction so it isn't embedded
    in a SOLID tile due to FP error), sets firer's grapple state to
    ATTACHED, fills anchor fields, computes initial rest length from
    pelvis distance (clamped to ≥80 px), and calls
    `mech_grapple_attach`. The grappled mech takes no damage. On
    lifetime expiry without a hit, `grapple_miss` fires and the
    firer is reset to IDLE. All gated on `w->authoritative` so
    clients don't fork their own state. New
    `projectile_find_grapple_head(pool, mid)` helper for the
    renderer.
  - **Fire path**. `mech_try_fire`'s WFIRE_GRAPPLE branch (was
    `NOT YET IMPLEMENTED`) is now the proper fire dispatch: edge-
    triggered (only on `BTN_FIRE` press); silently no-ops when
    `m->grapple.state != IDLE`; spawns the head from
    `R_HAND` toward `mech_aim_dir` at speed read from
    `wpn->projectile_speed_pxs` (1200 px/s, lifetime 0.5 s,
    gravity_scale 0); cooldown = `wpn->fire_rate_sec` (1.20 s).
    The weapon table entry for `WEAPON_GRAPPLING_HOOK` now carries
    the projectile-kind/speed/life/grav_scale fields for
    documentation + data-driven dispatch.
  - **Per-tick rope step + retract + release** (revised twice after
    user feedback). Original auto-contract pinned the firer at the
    anchor with no way out → replaced with a static-length
    one-sided rope (Tarzan swing) → user then asked for a
    player-driven retract ("hold W to zip up to the anchor") and
    flagged that ropes were too long to swing on. Final shape:
    - `projectile_step` clamps the hit-time rope length to
      `GRAPPLE_MAX_REST_LEN = 300 px` (initial-min 80 px so very
      close grapples still feel rope-y). Long-range fires now leave
      the firer "outside" the rope at attach time, so the constraint
      pulls them in to 300 px and they swing on a tight pendulum
      instead of dangling on a 600-px rope they couldn't actually
      swing on.
    - `mech_step_drive`'s server-side `state == ATTACHED` block:
      while `BTN_JET` (W) is held, decreases `rest_length` at
      `GRAPPLE_RETRACT_PXS = 800 px/s`, clamped at
      `GRAPPLE_MIN_REST_LEN = 60 px`. Releasing W stops the
      retract. Jet thrust still applies normally — the two effects
      reinforce upward when the anchor is above the firer.
    - SHOT_LOG: per-second `grapple_swing dist=… rest=… [retracting]`
      traces, plus edge-trigger `grapple_retract_start/_stop` lines
      so shot tests can verify the retract started/stopped on the
      right tick and the rope length actually moved.
    - Other releases unchanged: `BTN_USE` edge, anchor-mech death,
      firer death (`mech_kill`).
  - **Constraint kind**. Tile anchors use `CSTR_FIXED_ANCHOR`
    (`physics.c::solve_fixed_anchor`) which is now ONE-SIDED — pulls
    the particle in only when `d > c->rest`; slack (no force) when
    `d ≤ c->rest`. Mech anchors use `CSTR_DISTANCE_LIMIT` with
    `min_len = 0`, `max_len = rope_length` for the same one-sided
    behaviour (the existing `solve_distance_limit` already had the
    right shape). Together this gives the "Tarzan" feel: rope hangs
    naturally, you can drift closer to the anchor, and you snap taut
    when you swing past the rope length.
  - **Lifecycle helpers**. `mech_grapple_attach` appends a slot to
    the global `ConstraintPool` and stores its index on
    `m->grapple.constraint_idx`; `mech_grapple_release` flips the
    slot's `active = 0` (per spec, leakage is bounded by ~40
    grapple cycles per mech per round vs the 2048-slot pool — never
    pressures capacity).
  - **Snapshot**. New `SNAP_STATE_GRAPPLING = 1u << 12` in
    `state_bits`. When set, an 8-byte trailing suffix is appended
    to the `EntitySnapshot` wire (state, anchor_mech, anchor_part,
    reserved, anchor_x_q i16, anchor_y_q i16 — all 1 px res).
    Idle entities = 0 bytes overhead (the suffix is gated by the
    bit). Active grapple ≈ 240 B/s/active-grapple at 30 Hz.
    `snapshot_decode` validates the trailing bytes per-entity.
    `snapshot_apply` mirrors state to `m->grapple` for both local
    and remote mechs; clients never allocate a constraint (the pull
    arrives through pelvis-pos updates in subsequent snapshots).
    *(Diverges from the prompt's `SNAP_DIRTY_GRAPPLE = 1u << 11`
    on a per-entity dirty mask: the existing wire has no
    per-entity dirty mask, so the gating bit moved into the
    existing `state_bits` field which IS on the wire. Same intent
    — idle = 0 bytes.)*
  - **Renderer**. New `draw_grapple_rope(world, mid, alpha, off)`
    in `render.c`, called from `renderer_draw_frame` after each
    `draw_mech` (so the rope sits on top of the arm). FLYING →
    1.5 px line from R_HAND to the live `PROJ_GRAPPLE_HEAD` pos
    (looked up by `projectile_find_grapple_head`); ATTACHED → line
    from R_HAND to either `m->grapple.anchor_pos` (tile) or the
    target's bone particle (mech). Single straight line; flexing
    rope shader is M6 polish. Reconcile `visual_offset` threads
    through for the local mech.
  - **Edge handling**. Holding `BTN_FIRE` doesn't re-fire (edge
    gate). Re-press while FLYING is silently swallowed (no
    double-head). Re-press while ATTACHED auto-releases the current
    rope and falls through to fire a fresh head — this is the
    "chain grapple" behaviour the user needed for swinging across a
    level Tarzan-style. The 1.20 s `fire_rate_sec` cooldown still
    gates how fast chains can land; back-to-back fires need ≥72
    ticks between them. `BTN_USE` edge inside `mech_step_drive`
    releases. `mech_kill` releases first (before impulse) so a
    corpse doesn't keep dragging the rope.
  - **Shot tests**. Two scripts under `tests/shots/`:
    - `m5_grapple.shot` — basic fire/attach/release. Trooper at
      spawn (2240, 984) anchors to the wall column at L=468; the
      log captures `grapple_swing dist≈468 rest=468` traces while
      attached (proves no auto-contract); `grapple_release` at
      tick 180 on BTN_USE.
    - `m5_grapple_swing.shot` — pendulum + chain grapple. Trooper
      spawned mid-air at (2200, 600), fires/attaches, swings,
      releases (BTN_USE), re-fires, then performs a chain re-fire
      while ATTACHED — log shows `grapple_release` and a fresh
      `grapple_fire` on the same tick (auto-release-and-refire),
      followed by another attach. Verifies the user's "I want to
      swing like Tarzan, disconnect, fire and re-attach" flow.
    Contact sheet shows the rope rendering at ATTACHED + the
    mech visibly pulled to the wall by tick 150 + post-release
    at 190.
  - Verification: `make` clean; `make test-physics` ok;
    `make test-level-io` 25/25; `make test-pickups` 43/43;
    `make test-spawn` ok; `tests/net/run.sh` 13/13;
    `tests/net/run_3p.sh` 10/10; `make editor` builds clean.
    Protocol id stays `S0LG` (`SNAP_STATE_GRAPPLING` rides the
    existing u16 `state_bits`; the trailing 8 bytes are gated by
    the bit so idle-grapple bandwidth is unchanged).

- **P07 — Capture the Flag** (2026-05-04).
  - **New module** `src/ctf.{c,h}`. Public API: `ctf_init_round`,
    `ctf_step`, `ctf_drop_on_death`, `ctf_is_carrier`,
    `ctf_flag_position`. Server-authoritative; ctf operations mutate
    `world.flags[]` and set `world.flag_state_dirty` — the broadcast
    site is `main.c::broadcast_flag_state_if_dirty` (called once per
    `MATCH_PHASE_ACTIVE` tick), keeping ctf.c independent of net.c.
  - **Flag runtime data** in `world.h`: `FlagStatus` enum
    (HOME / CARRIED / DROPPED), `Flag` struct (home_pos, team,
    status, carrier_mech, dropped_pos, return_at_tick), `World.flags[2]`
    + `flag_count`. `FLAG_TOUCH_RADIUS_PX = 36`,
    `FLAG_AUTO_RETURN_TICKS = 30 * 60`,
    `FLAG_CAPTURE_DEFAULT = 5`. `World.match_mode_cached` mirrors
    `match.mode` so `mech.c` can branch in `mech_kill` without seeing
    `Game`. `World.flag_state_dirty` is the single hint bit.
  - **`ctf_init_round`** populates `flags[]` from `level.flags` (LvlFlag
    records) when mode == CTF and the level carries one Red and one
    Blue flag; index convention is `flags[0]=RED`, `flags[1]=BLUE`.
    Other modes (or invalid flag pair) → `flag_count = 0`. Both host
    and client run it from their respective round-start handlers.
  - **`ctf_step`** (server, per-tick): auto-return on timer, then
    36 px touch detection over every alive non-dummy mech.
    `ctf_touch` resolves the transition table (same-team HOME +
    carrying-enemy → capture; same-team DROPPED → return; enemy
    HOME/DROPPED → pickup; CARRIED skipped at outer loop).
    `ctf_capture` enforces the both-flags-home rule (capturing
    requires the toucher's own flag to be HOME — checked implicitly
    because the touch path fires only when `flag->status == HOME`).
    Capture: +5 to `match.team_score[carrier->team]`, +1 to
    `lobby.slots[scorer].score`, captured flag back to HOME.
  - **Carrier penalties** in `mech.c`:
    - `apply_jet_force` halves `thrust_pxs2` when `ctf_is_carrier`.
    - `mech_try_fire` rejects when `m->active_slot == 1 && ctf_is_carrier`
      (entire secondary disabled — see TRADE_OFFS).
  - **Death drops the flag**: `mech_kill` snapshots the pelvis position
    BEFORE the kill impulse displaces it, then calls
    `ctf_drop_on_death(world, match_mode_cached, mid, pelv)`. The
    drop transitions `status → DROPPED`, fills `dropped_pos`, sets
    `return_at_tick = world.tick + FLAG_AUTO_RETURN_TICKS`, and marks
    the dirty bit. Non-CTF modes are no-op.
  - **Wire protocol**: new message `NET_MSG_FLAG_STATE = 16` on
    `NET_CH_EVENT` (reliable, ordered). Variable size: 1 byte tag +
    1 byte flag_count + 12 bytes per flag (team / status /
    carrier_mech / pos_q / return_in_ticks). Max 26 bytes for a
    both-flags broadcast. `INITIAL_STATE` appends an optional
    flag-state suffix so future mid-round joiners see correct flag
    positions on connect (M4 still parks joiners in the lobby; this
    is forward-looking infra). Bandwidth: ~6 events/min × 26 B × 16
    peers ≈ 42 B/s aggregate, trivial vs the 5 KB/s/client budget.
    Helper functions `encode_flag_state` / `decode_flag_state` are
    shared between the broadcast and INITIAL_STATE paths.
  - **Mode-mask validation** in `main.c::start_round`: if rotation
    lands on CTF and the picked map's `META.mode_mask & MATCH_MODE_CTF`
    isn't set (or `level.flag_count != 2`), demote to TDM and log a
    warning. The check requires building the level to read META, so
    the CTF path takes one extra `map_build` per round when the
    rotation lands on a CTF entry — cheap (small maps, one
    arena-reset).
  - **CTF score limit default**: when mode == CTF and the config's
    `score_limit >= 25` (the FFA default), clamp to
    `FLAG_CAPTURE_DEFAULT = 5`. A host who explicitly sets
    `score_limit=10` in `soldut.cfg` keeps 10 (their config beats
    the default).
  - **Render** (`render.c::draw_flags` after `draw_mech`,
    inside `BeginMode2D`): vertical staff + triangular pennant
    team-colored. Position picks home_pos / dropped_pos /
    carrier-chest based on status (interp-lerped from `render_prev_*`
    for the carried case so the flag tracks moving bodies smoothly).
    Pennant gets a sin-driven tip-vertex wobble while CARRIED.
    DROPPED draws a faint outline halo to read as urgent.
  - **HUD** (`hud.c`): two new helpers, drawn first (so they show
    even for spectators / between rounds): `draw_flag_pips` puts a
    24-px pip in each top corner — Red TL, Blue TR — with HOME =
    solid, CARRIED = alpha-pulse, DROPPED = outline-only with
    center dot. `draw_flag_compass` projects each flag world pos
    via `GetWorldToScreen2D`; if off-screen, draws a small
    triangular arrow at the closest viewport edge pointing toward
    the flag, colored by team.  `hud_draw` signature widened to
    take `Camera2D camera`; `render.c` passes `r->camera`.
  - **`match.h`** comment on `MATCH_MODE_CTF` cleaned up — no longer
    "plays as TDM at M4".
  - **Regression test** `tests/ctf_test.c` (52 assertions,
    `make test-ctf`): init_round modes, flag_position cases, enemy
    pickup, friendly no-pickup, carrier-dies drop (CTF + non-CTF
    no-op), friendly return + scoring, auto-return on timer, capture
    + team/slot scoring, ctf_is_carrier truth, no-capture-without-
    carry guard. Authors a synthetic 2-flag level on top of
    `level_build_tutorial` so the test is self-contained.
  - **Crossfire CTF map (code-built)**: new `MAP_CROSSFIRE` MapId
    + `build_crossfire` in `maps.c`. 140×42 symmetric arena with
    Red base on the left, Blue base on the right, central cover,
    sniper overlooks, 2 flags at chest-height, 8 authored
    LvlSpawn records (4 per team), 5 example pickups. mode_mask
    = FFA|TDM|CTF. Existing maps got mode_mask = FFA|TDM (no CTF
    bit) so the runtime mode-mask validator demotes any CTF
    request landing on Foundry/Slipstream/Reactor to TDM with a
    log warning. The CTF score limit clamp's `>=25` heuristic
    works because the FFA default in `config_defaults` is 25; an
    explicit `score_limit=N<25` in `soldut.cfg` is honored.
  - **Team auto-balance** in start_round (`main.c`) and
    shotmode's start_round mirror: when mode is TDM/CTF, in-use
    slots get distributed RED/BLUE deterministically (slot 0 →
    RED, slot 1 → BLUE; explicit user picks survive a single-
    step imbalance). Without this, every player spawns on the
    FFA-default team (= RED) and CTF never triggers a touch
    transition because there's no "enemy carrier."
  - **Editor TOOL_FLAG** in `tools/editor/`. New `TOOL_FLAG`
    enum entry (with `F` key shortcut + toolbar button). Mouse-
    click drops a `LvlFlag` at the cursor; team auto-toggles
    1→2→1 between placements so a designer who clicks twice
    drops a Red+Blue pair. Right-click deletes nearest. Auto-sets
    `META.mode_mask` CTF bit when both teams' flags are present;
    clears when either drops below. Editor's `validate.c` already
    enforced the CTF constraints (≤1 flag per team, matching
    spawns); they continue to apply. Help modal updated to list
    the F shortcut + 1/2 team selection.
  - **Shot mode CTF support**: new `mode <ffa|tdm|ctf>` and
    `map <short_name>` directives in `src/shotmode.c`. `seed_world`
    honors them and runs `ctf_init_round` when in CTF mode (also
    sets `world.match_mode_cached` and `match.phase = ACTIVE`).
    New `flag_carry <flag_idx>` debug event for shot tests — pre-
    arms the local mech as a carrier without forcing the test to
    walk across the entire arena. `shotmode_run` now calls
    `config_load` so a soldut.cfg in the test cwd reaches
    `config_pick_*` (without this, networked shot tests couldn't
    select CTF mode/Crossfire map).
  - **Shot tests**:
    - `tests/shots/m5_ctf_capture.shot` — single-player end-to-
      end: spawn near RED base, arm BLUE flag carry via debug
      directive, walk into RED home flag, capture fires (R5/B0).
    - `tests/shots/m5_ctf_pickup.shot` — single-player touch-
      driven pickup: spawn 80 px west of BLUE flag, walk RIGHT
      into it, pickup transition fires + carrier visual.
    - `tests/shots/net/2p_ctf.{host,client}.shot` — paired
      networked: host (RED) arms a carry + walks RIGHT into RED
      home flag → capture fires + score 5-0; client (BLUE)
      mirrors flag-state via the wire, sees BLUE flag missing
      from base during carry, sees ROUND OVER post-capture.
    - `tests/shots/net/run_ctf.sh` — wraps `tests/shots/net/run.sh`
      with a temporary `soldut.cfg` that drives mode=ctf +
      map_rotation=crossfire. 12 base assertions (host/client
      plumbing) + 7 CTF-specific (flag init, capture, mirror).
  - **Network smoke test**: `tests/net/run_ctf.sh` (15 assertions)
    — log-driven, CI-runnable. Verifies host builds Crossfire
    with mode_mask=0x7, both sides run `ctf_init_round`, team
    auto-balance fires (red=1 blue=1), CTF score limit clamps
    to 5, client receives ROUND_START with mode=CTF, snapshots
    flow, round ends correctly.
  - Verification: `make` clean; `make test-physics` ok;
    `make test-level-io` 25/25; `make test-pickups` 43/43;
    `make test-ctf` 52/52; `make test-spawn` ok; `make test-editor`
    (editor smoke + 4 scenario scripts) ok;
    `tests/net/run.sh` 13/13; `tests/net/run_3p.sh` 10/10;
    `tests/net/run_ctf.sh` 15/15;
    `tests/shots/net/run.sh 2p_basic` 12/12;
    `tests/shots/net/run.sh 2p_motion` 12/12;
    `tests/shots/net/run_ctf.sh` 12+7=19/19. Protocol id stays
    `S0LG` (`NET_MSG_FLAG_STATE` uses message id 16, no
    entity-snapshot widening).

  - **Lobby UX pass — host setup, mode/map controls, per-player team picker** (post-P07).
    - **MODE_HOST_SETUP screen** (`host_setup_screen_run` in
      `lobby_ui.c`). Title's "Host Server" button now routes to a
      pre-lobby setup screen instead of dropping straight into the
      lobby. The screen's mode picker (FFA / TDM / CTF radio buttons),
      map cycle button (auto-skips maps whose `meta.mode_mask` doesn't
      cover the picked mode), score-limit + time-limit steppers, and
      friendly-fire toggle land in `g->config` + a fresh `match_init`
      when the user clicks **Start Hosting**. The script-driven test
      paths (`--shot ... network host`) bypass the screen and continue
      to read directly from `soldut.cfg`.
    - **Lobby MATCH panel** at the top of the lobby (between LOBBY
      title and player list). For all viewers: the current mode is
      a highlighted pill, the map name is visible, score/time/ff
      printed beside. For the host (only when `match.phase == LOBBY ||
      SUMMARY`, never mid-round): the FFA/TDM/CTF pills + map button
      are clickable. Host changes mutate `g->match` + `g->config` and
      broadcast `NET_MSG_LOBBY_MATCH_STATE` so all clients see the new
      mode/map immediately. Mode changes auto-skip to a compatible map
      and clamp the CTF default score limit to 5 when bumping from
      FFA/TDM's default of 25.
    - **TEAM panel at the top of the loadout column** (above LOADOUT,
      not buried beneath it as before). Mode-aware:
      - TDM/CTF: three-cell **RED / BLUE / Spec** picker. The active
        team is highlighted in its team color; the player taps any
        cell to switch. Each player controls only their own slot;
        clicks route through the existing `apply_team_change` path
        (`net_client_send_team_change` for clients, `lobby_set_team`
        for the host) which the server fans out via the standard
        dirty-bit lobby_list broadcast.
      - FFA: a two-cell **Playing / Spectator** toggle (FFA has only
        one team — the meaningful axis is "in" vs "sitting out").
    - **Bootstrap respect for cfg**: `networked_shot_bootstrap`
      stopped overriding `auto_start_seconds` / `time_limit` /
      `score_limit` when a `soldut.cfg` was loaded — same shape as the
      P07 fix for CTF tests; lets the new team-change shot test set a
      4-second auto-start for a longer lobby observation window.
    - **Test scaffold**: new `team_change <team>` shotmode directive
      drives `net_client_send_team_change` for clients (and
      `lobby_set_team` for hosts) so a paired shot script can simulate
      the lobby UI's TEAM-button click without a real mouse. Coverage:
      - `tests/shots/net/2p_team_change.{host,client}.shot` — paired
        scripts; client sends BLUE at lobby tick 120, host's player
        list reflects it before the round starts. Captures
        before/after PNGs + a side-by-side contact sheet.
      - `tests/shots/net/run_team_change.sh` — wraps `run.sh` with a
        TDM-mode `soldut.cfg`. 12 base assertions (round flow + wire
        round-trip) + 4 team-change-specific (host config TDM, slot 1
        accepted, client team_change sent, lobby_list received). 16/16
        passing.
    - Verification: full pre-existing test matrix re-runs clean —
      `tests/net/run.sh` 13/13, `run_3p.sh` 10/10, `run_ctf.sh` 15/15,
      `run.sh 2p_basic` 12/12, `run.sh 2p_motion` 12/12,
      `run_ctf.sh` (shot) 19/19, `run_team_change.sh` 16/16.

  - **CTF combat regression — auto-balance ordering bug** (post-P07).
    - **Bug**: `start_round` ran the TDM/CTF team auto-balance AFTER
      `lobby_spawn_round_mechs`. The spawn helper bakes `slot.team`
      into `mech.team`; with the auto-balance running too late, both
      players spawned on the FFA-default RED team and
      `mech_apply_damage`'s friendly-fire gate (`shooter.team ==
      victim.team && !world.friendly_fire`) silently dropped every
      shot. CTF/TDM rounds played as "everyone on RED, can't shoot
      anyone." The user reported it as "shooting opposing mechs in
      CTF mode does nothing."
    - **Fix**: moved the auto-balance block above
      `lobby_spawn_round_mechs` in `main.c::start_round`. Mechs now
      get spawned with their post-balance team. Comment marked the
      ordering as load-bearing.
    - **Regression test**: `tests/shots/net/2p_ctf_combat.{host,client}.shot`
      + `run_ctf_combat.sh` — 12 base + 7 CTF-combat assertions
      (build, mode, balance ran, balance precedes mech_create, hit
      lands, victim HP drops, fire-event log shows opposing-team
      hit). 19/19 passing.
    - **Drop-on-kill paired shot**: `tests/shots/net/2p_ctf_drop_on_kill.{host,client}.shot`
      + `run_ctf_drop_on_kill.sh` — 12 base + 4 drop assertions
      (server-side carrier arming, mech_kill on victim id=1,
      `ctf: drop flag=0` log line, drop pos near spawn). 16/16
      passing. Verifies the wire round-trip that `ctf_test.c`'s unit
      `test_carrier_dies_drop` covered at the function level.
    - **New shotmode debug directives** (test-only):
      - `arm_carry <flag_idx> <mech_id>` — host-side server-state
        flag arming; needed because `flag_carry` on a *client* only
        mutates client-local state and never reaches the server.
      - `kill_peer <mech_id>` — host-side `mech_apply_damage` with
        9999 dmg from shooter=-1 (environmental). Sidesteps weapon
        accuracy variance (recoil + bink) so death-flow tests are
        deterministic.

  - **Editor → game CTF round-trip via F5** (post-P07).
    - **`--test-play` auto-detects CTF mode** from the loaded
      `.lvl`'s `META.mode_mask` + `flag_count`. The host-setup screen
      doesn't reach the F5 path (the editor forks `./soldut
      --test-play <abs_path>` directly), so the runtime peeks the
      level's META right after parsing args: if `(mode_mask & CTF) &&
      flag_count == 2`, it sets `config.mode = CTF`,
      `config.score_limit = FLAG_CAPTURE_DEFAULT (= 5)`, and
      `config.friendly_fire = false`. Without this, F5 on a CTF map
      hardcoded FFA — flags rendered but capture never fired because
      `match.mode != CTF` blocked `ctf_step`. Logs
      `test-play: detected CTF map (mode_mask=0x4, flag_count=2)`
      so the auto-detect is observable.
    - **Editor `flag_add` auto-sets `META.mode_mask` CTF bit** when
      both team-1 and team-2 flags are present, mirroring the
      `tool_flag` (mouse-click) path. Editor shot scripts that drop
      a CTF flag pair now save a CTF-tagged map without an extra
      `meta_set` step.
    - **`tools/editor/shots/ctf_map.shot`** — programmatically
      authors a 50×16 tile arena: floor + side walls + Red flag
      at (200, 332) + Blue flag at (1400, 332) + matching team
      spawns. Saves to `assets/maps/ctf_test.lvl`. Reloads from
      disk and asserts the round-trip preserved 2 flags + 2 spawns.
    - **`tests/shots/m5_ctf_editor_map.shot`** — single-player
      shot that loads the editor's `ctf_test.lvl`, sets `mode ctf`,
      spawns the player just east of the BLUE flag. Player walks
      LEFT — touches BLUE flag (pickup), keeps walking ~1280 px to
      RED home flag (capture). Final score R5/B0 (CTF default).
      Visual contact sheet shows pickup → carry → capture → both-
      home transitions cleanly.
    - **`tests/test_play_ctf.sh`** — verifies the actual F5 code
      path: spawns `./soldut --test-play assets/maps/ctf_test.lvl`
      with a 5-second timeout, greps `soldut.log` for the
      auto-detect log line + round-begin in CTF mode +
      ctf_init_round + score_limit clamp.
    - **`make test-ctf-editor-flow`** chains both halves: editor
      shot writes the .lvl, game shot exercises the capture flow,
      then the F5 / --test-play check runs. 4 (shot) + 4 (test-play)
      = 8/8 assertions all green; 4 unit assertions inside the
      editor shot itself (saved-flag count, mode_mask CTF bit set,
      reload preserves flags + spawns).

  - **Snapshot pos quant 8× → 4×** (post-P07, 2026-05-05).
    - **Bug**: `quant_pos` in `src/snapshot.c` (and the parallel
      encoders in `src/net.c` for projectile state, hit/fire events,
      flag state) packed world positions as `int16_t` with an 8×
      sub-pixel factor. Range was ±4096 px, resolution 0.125 px.
      Crossfire CTF is 4480 px wide, so anything east of x=4096 (the
      entire BLUE base, including the BLUE flag at x=4160) silently
      wrapped to x=4095 in the wire format. User-visible symptom was
      "the client gets stuck on their own flag" — every snapshot
      jammed the client's local mech back to x=4095 even as the
      server simulated past it.
    - **Fix**: cut the factor in half to 4×. Range now ±8190 px,
      resolution 0.25 px (still well below renderer-interp jitter).
      Logged in TRADE_OFFS.md; revisit when a map exceeds ~8000 px.
    - Verification: paired host+client CTF shot tests on Crossfire
      no longer pin the client to x=4095; capture flow completes
      end-to-end on the wide map.

- **P08 — Map sharing across the network** (2026-05-05).
  - **New modules**: `src/map_cache.{h,c}` (content-addressed cache
    at platform-specific path, 64 MB LRU eviction by mtime, atomic
    `<crc>.lvl.tmp` + rename writes) and `src/map_download.{h,c}`
    (per-process MapDownload struct on permanent arena: 2 MB buffer +
    1792-bit chunk-received bitmap, in-order reassembly, duplicate
    detection, 30 s stall watchdog).
  - **Wire format additions** (all on `NET_CH_LOBBY`, reliable):
    - `MapDescriptor` (36 bytes: u32 crc32 + u32 size_bytes + u8
      short_name_len + char[24] short_name + u8[3] reserved) appended
      to every `NET_MSG_INITIAL_STATE` body. crc=0 size=0 = code-built
      fallback (no .lvl on disk to ship).
    - `NET_MSG_MAP_REQUEST = 40` (9 bytes: tag + crc32 + resume_offset)
      from client to server.
    - `NET_MSG_MAP_CHUNK = 41` (16-byte header + ≤1180 byte payload).
      Header: tag + crc32 + total_size + chunk_offset + chunk_len +
      is_last + reserved. The 1180-byte payload sizing matches the
      1200-byte ENet MTU after framing + chunk header.
    - `NET_MSG_MAP_READY = 42` (8 bytes: tag + crc32 + status +
      reserved[2]) from client to server. Status: 0=ok, 1=crc_mismatch,
      2=parse_failure, 3=too_large.
    - `NET_MSG_MAP_DESCRIPTOR = 43` (1 + 36 bytes) broadcast on
      mid-lobby host map changes so clients re-resolve.
  - **Cache directory** by platform (`map_cache_init` resolves +
    `mkdir -p` once per process; cache dir override via `XDG_DATA_HOME`
    on Linux):
    - Linux: `$XDG_DATA_HOME/soldut/maps/` (default `~/.local/share/soldut/maps/`)
    - macOS: `~/Library/Application Support/Soldut/maps/`
    - Windows: `%APPDATA%\Soldut\maps\`
  - **Client resolve order** (`net.c::client_resolve_or_download`):
    1. `assets/maps/<short>.lvl` with matching CRC + size → MAP_READY ok.
    2. `<cache>/<crc>.lvl` → MAP_READY ok.
    3. `map_download_begin` + send MAP_REQUEST.
  - **Server-side stream** (`net.c::server_handle_map_request`): opens
    `g->server_map_serve_path`, fseek to `resume_offset`, streams 1180-byte
    chunks via `enet_send_to(NET_CH_LOBBY, RELIABLE)` until EOF. ENet
    handles backpressure on the reliable channel.
  - **Host map-ready gate** (main.c `host_match_flow_step`): `lobby_tick`'s
    auto-start fire is held above 1.5 s while
    `net_server_all_peers_map_ready(current_map_desc.crc32)` is false.
    Code-built maps (crc=0) bypass the gate. Slow downloaders DO
    extend the countdown; very slow downloaders eventually time out
    via the client-side 30 s stall watchdog.
  - **Serve-info refresh sites**: `bootstrap_host` (initial map),
    `start_round` (per-round map), `lobby_screen_run` mode-change branch,
    `lobby_screen_run` map-cycle branch. All call
    `maps_refresh_serve_info` then broadcast `NET_MSG_MAP_DESCRIPTOR`
    when on the host.
  - **Client-side ROUND_START path**: `client_handle_round_start` now
    calls `map_build_for_descriptor` (new helper in maps.c) which walks
    assets-with-matching-CRC → cache-by-CRC → fallback to MapId code-build.
    Downloaded maps load from cache on round start without polluting
    the assets directory.
  - **Lobby UI**: a thin progress strip with bar + percentage shows in
    `lobby_screen_run` whenever `g->map_download.active`. Layout fits
    above the existing MATCH panel; team / loadout controls remain
    interactive (the host's gate prevents accidental round start before
    download completes).
  - **Trust model** (per `documents/m5/10-map-sharing.md` §"Trust"):
    `MapDescriptor.size_bytes > 2 MB` → MAP_READY status=TOO_LARGE
    (and no download begun). MAP_CHUNK with offset+len > total_size →
    rejected in `map_download_apply_chunk`. Reassembled buffer's CRC
    must match descriptor; mismatch → MAP_READY status=CRC_MISMATCH +
    log + don't write cache. Cache LRU evicts at 64 MB; one-shot
    `map_cache_evict_lru` runs after each successful write.
  - **Tests**: new `tests/map_chunk_test.c` (21 assertions,
    `make test-map-chunks`) covering chunk-stream reassembly,
    duplicate detection (bytes_received doesn't double-count), OOB
    rejection, non-aligned-offset rejection, bit-flip CRC failure.
    New `tests/synth_map.c` writes synthetic .lvl on disk.
    New `tests/net/run_map_share.sh` (15 assertions) end-to-end host
    serves + client downloads + writes cache + plays the round.
    Verifies MAP_REQUEST flow, chunk streaming, MAP_READY ack, and the
    cached `<crc>.lvl` file appearing in the client's XDG_DATA_HOME.
  - **Version**: bumped `SOLDUT_VERSION_STRING` to `0.0.7-m5p08`.
    Protocol id stays `S0LG` (P08 is additive — no widening of any
    existing message body).
  - Verification: `make` clean; `make test-physics` ok;
    `make test-level-io` 25/25; `make test-pickups` 43/43;
    `make test-ctf` 52/52; `make test-spawn` ok; `make test-map-chunks`
    21/21; `tests/net/run.sh` 13/13; `tests/net/run_3p.sh` 10/10;
    `tests/net/run_ctf.sh` 15/15; `tests/net/run_map_share.sh` 15/15.

- **P08b — Custom map registry: scan `assets/maps/`, surface in lobby UI** (2026-05-05).
  - **Static `g_maps[MAP_COUNT]` → runtime `g_map_registry`**.
    `MAP_COUNT` becomes `MAP_BUILTIN_COUNT = 4` (the reserved indices
    `MAP_FOUNDRY..MAP_CROSSFIRE` for `build_fallback`'s switch). New
    `MapRegistry { entries[MAP_REGISTRY_MAX=32]; count; }` populated
    by `map_registry_init` at `game_init` time. New `MapDef` struct
    grows: `short_name[24]` + `display_name[32]` + `blurb[64]` +
    `tile_w/h` + `mode_mask` + `has_lvl_on_disk` + `file_crc` +
    `file_size`. Builtins always seed entries 0..3; custom entries
    appear at index >= 4.
  - **Init order** (in `game_init`): arenas → particle/constraint
    pools → fx/projectile pools → `config_defaults` →
    `map_download_init` → `map_cache_init` → `map_registry_init` →
    LOG_I "ok". Critically, registry runs BEFORE `config_load` (which
    happens in `main.c`) so `map_rotation=my_arena` resolves through
    `map_id_from_name` against the freshly-populated registry.
  - **Cheap META scan** — `scan_one_lvl` reads the `.lvl` 64-byte
    header (magic check at `[SDLV]`, world_w/h at offset 16/20,
    STRT_OFF at 28, CRC32 at 36) + walks the lump directory looking
    for the META tag (32 bytes, name_str_idx at offset 0, mode_mask
    at offset 12) + reads the display name out of the STRT lump.
    No `level_load` per file — sub-millisecond per scan even on
    spinning disks at our map count.
  - **Builtin override semantics**: a disk file matching a builtin's
    short_name (e.g. saving `assets/maps/foundry.lvl` from the
    editor) overwrites the builtin's CRC + size + mode_mask but
    keeps the reserved index. The display name from META wins; if
    META has no name string the builtin's "Foundry" stays. The
    builtin's blurb is preserved (the .lvl format doesn't carry
    blurb text — STRT's blurb_str_idx is read by level_load but
    P08b's scan doesn't surface it).
  - **Custom-name display fallback** — display_name is META string
    if present, else titlecased short_name (`my_arena` → `My Arena`,
    splitting on `_` and `-`). `map_id_from_name` walks the registry
    by short_name AND display_name (so `map_rotation=my_arena` and
    `map_rotation="My Arena"` both resolve).
  - **Custom-map fallback path in `map_build`** — for IDs <
    `MAP_BUILTIN_COUNT`, missing/corrupt `.lvl` falls back to
    `build_fallback`'s code-built switch as before (LOG_I for
    file-not-found, LOG_W for any other failure). For custom IDs
    (>= 4) — those exist in the registry only because a `.lvl` was
    on disk at scan time — a runtime load failure means the file was
    deleted or corrupted between scan and play. Hard-fall-back to
    Foundry with LOG_E so the round still starts but the regression
    is visible in logs.
  - **Lobby UI walks**: three sites in `lobby_ui.c` rewritten to
    iterate `g_map_registry.count` instead of `MAP_COUNT`:
    `setup_next_map_for_mode` (host-setup screen mode→map cycle),
    in-lobby mode-change auto-pick-compatible-map branch, and the
    in-lobby map cycle button. Display gains a fall-through to
    `g->pending_map.short_name` when `match.map_id` is out of the
    client's local registry range — happens on the moment a client
    first connects to a host running a custom map the client has
    never seen.
  - **Tests**:
    - `tests/map_registry_test.c` (`make test-map-registry`,
      ~25 assertions across 7 cases): empty dir → 4 builtins; one
      custom .lvl → 5th entry with CRC/size populated; foundry.lvl
      override preserves slot 0 but stamps disk metadata; malformed
      file skipped; cap of 32 honored; NULL/missing dir → builtins;
      idempotent re-init shrinks back when scan dir empties out.
    - `tests/shots/net/run_meet_named.sh` (`make test-meet-named`,
      18 assertions): editor authors `my_arena.lvl` (NOT
      overwriting a builtin), host's `soldut.cfg` carries
      `map_rotation=my_arena`, host log shows
      `map_registry: + my_arena` and `match: round begin ... map=4`
      (custom slot), client downloads via P08, both walk + meet,
      caches `<crc>.lvl` in client's `XDG_DATA_HOME`. Companion to
      the existing `test-meet-custom` (which exercises the override
      path by saving as `foundry.lvl`).
  - Resolves the "Custom map names not in lobby rotation (P08
    follow-up)" trade-off — entry deleted from `TRADE_OFFS.md`.
  - Verification: `make` clean; `make test-physics` ok;
    `make test-level-io` 25/25; `make test-pickups` 43/43;
    `make test-ctf` 52/52; `make test-spawn` ok; `make test-map-chunks`
    21/21; `make test-map-registry` ok; `make test-meet-custom` ok;
    `make test-meet-named` 18/18; `tests/net/run.sh` 13/13;
    `tests/net/run_3p.sh` 10/10; `tests/net/run_ctf.sh` 15/15;
    `tests/net/run_map_share.sh` 15/15. Protocol id stays `S0LG`
    (P08b is host-local — no wire changes).

- **P09 — `BTN_FIRE_SECONDARY` + host controls + bans.txt + vote picker** (2026-05-05).
  - **`BTN_FIRE_SECONDARY` (RMB).** New input bit `1u << 12` in
    `src/input.h`; `src/platform.c::platform_sample_input` fills it
    from `MOUSE_BUTTON_RIGHT`. Edge-triggered (one shot per RMB press)
    while the existing `BTN_FIRE` stays level-triggered for full-auto
    weapons.
  - **`fire_other_slot_one_shot` in `src/mech.c`.** Static helper above
    `mech_try_fire`. Pattern: save active-slot aliases (`m->weapon_id`
    / `m->ammo` / `m->ammo_max`) → swap to inactive slot's stats →
    dispatch by `wpn->fire` (HITSCAN → `weapons_fire_hitscan`;
    PROJECTILE / SPREAD / THROW / BURST → `weapons_spawn_projectiles`
    with the burst-pending queue for BURST; MELEE → `weapons_fire_melee`;
    GRAPPLE → mirrors the inline grapple branch from `mech_try_fire`,
    spawning `PROJ_GRAPPLE_HEAD` and setting `m->grapple.state =
    GRAPPLE_FLYING`) → decrement the inactive slot's ammo bucket →
    restore active-slot aliases. Charge weapons (`charge_sec > 0`) are
    rejected — RMB is press-only. CTF carrier penalty applies to
    `other_slot == 1` regardless of which slot is currently active, so
    a flag carrier's secondary remains disabled via either fire path.
    Shared `fire_cooldown` gates LMB+RMB on the same tick.
  - **Hook in `mech_try_fire`.** BTN_FIRE_SECONDARY edge check sits at
    the very top of the function, above the active-slot guards (alive
    + cooldown + reload + carrier). Order matters: the inactive
    primary stays usable when the active secondary is gated by the
    carrier rule. The active-slot dispatch below sees the cooldown set
    by the inner `weapons_fire_*` and bails — neither path doubles DPS.
  - **`bans.txt` persistence.** New `lobby_load_bans` /
    `lobby_save_bans` in `src/lobby.{c,h}`. Flat one-entry-per-line
    format: `<addr_hex> <name>`, blank lines + `#`-comments tolerated.
    `lobby_load_bans` records the path on
    `LobbyState.ban_path[LOBBY_BAN_PATH_BYTES = 96]` so subsequent
    `lobby_ban_addr` calls auto-save (re-write the whole file; bans
    cap at 32 so this is trivial). Hooked into `bootstrap_host` after
    network setup, before any peers can connect. Unit tests don't set
    a path → no disk I/O during test runs.
  - **Host-side kick/ban entry.** `server_handle_lobby_kick_or_ban` in
    `src/net.c` factored: the wire-driven path now validates host
    status and calls a new public `net_server_kick_or_ban_slot(ns, g,
    target_slot, ban)` that does the actual ban + disconnect + chat
    notification. The host's own UI calls the public directly without
    wire round-trip (mirrors the `apply_team_change` / `apply_ready_toggle`
    pattern in `lobby_ui.c`).
  - **Lobby UI: kick/ban buttons + confirmation modal.**
    `PlayerListCtx` grows two fields (`LobbyUIState *ui_state`,
    `bool host_view`). `player_row` renders compact `[Kick]` `[Ban]`
    buttons on the right edge of every non-host row when `host_view`
    is true; click stages `L->kick_target_slot` / `L->ban_target_slot`
    (only one set at a time). After the list iteration,
    `lobby_screen_run` renders a centered confirmation modal with a
    160-alpha dim overlay, the target's name in the title, "(persists
    across host restarts)" subtitle for ban, and explicit Cancel /
    Confirm buttons. Confirm calls `apply_kick_or_ban(g, slot, ban)`
    which routes to `net_client_send_kick/ban` (clients) or
    `net_server_kick_or_ban_slot` (host).
  - **Three-card map vote picker.** New `host_start_map_vote` in
    `src/main.c` walks `g_map_registry.count`, collects entries that
    support the current `match.mode` (`mode_mask & (1u << mode)`)
    excluding the just-played map, Fisher-Yates shuffles, picks up
    to 3 candidates. Calls `lobby_vote_start(L, a, b, c, dur)` with
    `dur = summary_remaining * 0.8` so the winner is decided before
    `begin_next_lobby` fires. Broadcasts `NET_MSG_LOBBY_VOTE_STATE`
    immediately. No-op on offline solo or when no compatible maps
    other than the current one. `host_match_flow_step` now calls
    `lobby_tick` during `MATCH_PHASE_SUMMARY` so the vote_remaining
    countdown actually decays. `begin_next_lobby` reads
    `lobby_vote_winner` (popcount-based) and overrides
    `g->match.map_id` before the LOBBY broadcast — clients see the
    winner via the standard `net_server_broadcast_match_state` /
    `lobby_list` flow.
  - **Vote-card UI in `summary_screen_run`.** When a vote is active
    (any of `vote_map_a/b/c` >= 0), the scoreboard rect shrinks to
    leave a `S(180)` strip above the bottom margin. Each card:
    `S(220)`-wide, side-by-side, with a placeholder gray thumbnail
    (real art at P13/P16), the map's `display_name` and `blurb`
    (sourced from `MapDef`), live tally (popcount of the candidate's
    `vote_mask`), and a per-card "Vote" button. The local slot's
    chosen card is bordered + tinted green and labelled "VOTED". New
    `apply_map_vote(g, choice)` helper mirrors the team-change pattern
    — host calls `lobby_vote_cast` directly + rebroadcasts;
    client sends `NET_MSG_LOBBY_MAP_VOTE`.
  - **Title-screen Controls modal.** Small "Controls" button in the
    bottom-right corner of `title_screen_run` toggles
    `LobbyUIState.show_keybinds`. Modal is a 520×440 panel with a
    13-row keybind table sourced from `src/platform.c` (the canonical
    source of truth — code wins over docs per CLAUDE.md). Lists the
    actual game keys (`X` for prone, `F` for melee, NOT the
    `documents/m5/13-controls-and-residuals.md` placeholders Z/V),
    with Right Mouse → "Secondary fire — inactive slot, NEW". Close
    via the Close button or Esc.
  - **Shotmode wiring.** `tests/shots/m5_secondary_fire.shot` — a
    Trooper with Pulse Rifle + Frag Grenades fires LMB at T15 (rifle
    hitscan, ammo 30→29), again at T23 (29→28), then RMB-taps at T40
    (`spawn_proj wpn=10 kind=7` for the Frag Grenade) — active slot
    stays primary throughout (ammo cap stays at 30). After the Frag
    Grenade's 0.6 s `fire_rate_sec` clears, LMB resumes at T85 with
    `fire mech=0 wpn=0` — proves the active slot wasn't flipped by
    the RMB tap. New shotmode button name `fire_secondary` →
    `BTN_FIRE_SECONDARY`. New SHOT_LOG line in `mech_try_fire` on
    every BTN_FIRE_SECONDARY edge: `mech=N fire_secondary edge
    active=A prim=P sec=S cd=…`.
  - **Resolves three M4-era trade-offs** (deleted from `TRADE_OFFS.md`):
    "Map vote picker UI is partial", "Kick / ban UI not exposed",
    "`bans.txt` not persisted".
  - Verification: `make` clean; `make test-physics` ok;
    `make test-level-io` 25/25; `make test-pickups` 43/43;
    `make test-ctf` 52/52; `make test-spawn` ok; `make test-map-chunks`
    21/21; `make test-map-registry` ok; `tests/net/run.sh` 13/13;
    `tests/net/run_3p.sh` 10/10; `tests/net/run_ctf.sh` 15/15;
    `tests/net/run_map_share.sh` 15/15. Protocol id stays `S0LG`
    (P09 is additive on `NET_MSG_LOBBY_VOTE_STATE` / `LOBBY_KICK` /
    `LOBBY_BAN`, all already wired).
  - **Post-P09 sync follow-up** (same day): user playtest reported the
    grappling hook "disappearing" mid-flight and remote fires looking
    inconsistent. Three gaps surfaced — two were pre-P09 (P06 / earlier
    M2) but only became visible once RMB made secondary fires routine:
    1. **`weapons_record_fire` was never called for `WFIRE_GRAPPLE`.**
       Both the existing LMB-grapple branch in `mech_try_fire` and P09's
       new `fire_other_slot_one_shot` GRAPPLE case now emit a
       `FIRE_EVENT` so remote clients learn about the head spawn.
       Pre-existing P06 oversight; the static `record_fire` helper in
       `weapons.c` was renamed to public `weapons_record_fire` and
       declared in `weapons.h`.
    2. **`client_handle_fire_event` had no `WFIRE_GRAPPLE` case.**
       Remote clients (and the firer themselves, see below) never spawned
       a visual `PROJ_GRAPPLE_HEAD`, so the rope rendered nothing during
       FLYING (it draws hand → head, but no head existed on the client).
       New branch spawns a visual-only head; `projectile_step`'s alive=0
       paths run unconditionally so the head dies cleanly on tile/bone
       hit. The server still owns attach via `w->authoritative`.
    3. **Firer's own non-hitscan visuals were dropped.** The
       `if (shooter == local_mech_id) return;` guard at the top of
       `client_handle_fire_event` skipped the FIRE_EVENT for the firer
       on the assumption that the local predict path
       (`weapons_predict_local_fire`) had already drawn it. But predict
       only handles HITSCAN; for PROJECTILE/SPREAD/BURST/THROW/GRAPPLE,
       the firer saw nothing of their own shot. Restructured the
       handler: skip-self applies only to HITSCAN+MELEE (where predict
       drew the tracer); for projectile/grapple kinds, the visual is
       spawned for self too (predict's sparks+recoil aren't duplicated
       because the muzzle-spark loop is now `if (!is_self)` gated).
  - **Shot-test scaffold gap fixed too.** Networked shotmode never
    drained `firefeed` / `hitfeed` / `pickupfeed` — only `killfeed`
    via `shot_apply_new_kills`. So `FIRE_EVENT` / `HIT_EVENT` /
    `PICKUP_STATE` never crossed the wire in shot tests, masking the
    P09 grapple bug from the existing test scaffold. Added
    `shot_broadcast_new_hits` / `_fires` / `_pickups` to `shotmode.c`
    and call them from `shot_host_flow`'s `MATCH_PHASE_ACTIVE` branch.
  - **New paired shot test** `tests/shots/net/2p_secondary_fire.{host,client}.shot`
    + `run_secondary_fire.sh` (12 base + 7 RMB-specific = 19/19): host
    holds Pulse Rifle + Frag Grenades, client holds Pulse Rifle +
    Grappling Hook. Each side RMB-fires the inactive slot. Asserts:
    host's spawn_proj for the Frag Grenade, host's BTN_FIRE_SECONDARY
    edge for both mechs (own and the client's), server-side
    `grapple_fire(RMB)` for the client's RMB, client-side
    `client_fire_event grapple shooter=N self=1` SHOT_LOG showing the
    visual head spawned for the firer themselves, and active-slot
    persistence (ammo_max stays at 30 = primary's mag throughout).
  - **Post-P09 kick/ban follow-up** (same day): user playtest reported
    the kick UI was poorly displayed AND that kicked players didn't
    actually leave. Three bugs:
    1. **Client never returned to title on forced disconnect.** ENet
       emits `ENET_EVENT_TYPE_DISCONNECT` on the client when the host
       calls `enet_peer_disconnect_later`, which clears `ns->connected`
       — but nothing in the main loop watched that flag. The kicked
       client just sat there with no server. Added a hook in `main.c`
       (and a parallel one in `shotmode.c`'s networked loop for
       testability): when `role == NET_ROLE_CLIENT && !ns->connected`,
       tear down net + lobby state and return to MODE_TITLE.
    2. **Modal layout was rendering before the chat / loadout panels**,
       so the chat panel painted over the lower half of the modal
       (including the buttons) — that's why the user said "GUI
       display is bad." Moved the modal block to the very end of
       `lobby_screen_run`. Pixel inspection confirmed the buttons now
       render at their intended colors instead of ~30% via the chat
       panel's overlay.
    3. **Modal lacked visual hierarchy.** Redesigned with an accent
       top-stripe + border (orange for kick, red for ban), a
       descriptive subtitle ("Disconnect this player. They can rejoin
       freely." vs "Saved to bans.txt; persists across host
       restarts."), explicit slate Cancel and full-saturation
       destructive Confirm buttons sized 170×48. Player-row buttons
       went 56×20→72×28 with brighter hover/border colors.
  - **New shotmode directives** for kick/ban testing:
    - `at <tick> kick <slot>` / `ban <slot>` — host-side, drives
      `net_server_kick_or_ban_slot` directly (mirrors the production
      lobby UI's confirmation modal).
    - `at <tick> kick_modal <slot>` / `ban_modal <slot>` — host-side,
      sets `LobbyUIState.kick_target_slot/ban_target_slot` so the
      modal renders for layout-regression screenshots.
  - **`lobby_chat_system` now logs** the system message via `LOG_I`
    so kick/ban / "X joined" / "Y left" announcements are visible in
    `soldut.log` for tests + post-mortems (chat ring contents alone
    aren't reachable from a log file).
  - **Networked shotmode now calls `lobby_load_bans("bans.txt")`** in
    `networked_shot_bootstrap` so shot tests exercise the auto-save +
    reload-on-restart paths (without this, the ban directive's
    auto-save wouldn't fire — `lobby_ban_addr` only writes when
    `L->ban_path` is non-empty).
  - **New 3-player kick/ban test** `tests/shots/net/run_kick_ban.sh`
    (20/20 — 17 phase-1 + 3 phase-2): host + ClientB + ClientC,
    scripted `kick 1` then `ban 2`. Phase 1 asserts both clients
    receive forced disconnect, host's chat ring records the kick/ban
    announcement, peer disconnect events fire, `bans.txt` contains
    ClientC (and NOT ClientB, since kick != ban). Phase 2 starts a
    fresh host that reads the persisted `bans.txt`, ClientC tries to
    reconnect, server logs "banned — rejecting" and the client
    handshake errors out at `REJECT (bad challenge)`.
  - **New layout-regression test** `tests/shots/net/run_kick_modal.sh`
    captures the kick + ban modal screenshots so subsequent UI
    regressions are visible. Uses the new `kick_modal` / `ban_modal`
    directives + a temp `soldut.cfg` with `auto_start_seconds=20` so
    the lobby stays open long enough.
  - **Post-P09 round-loop refactor + multi-round match flow** (same
    day): user playtest reported "killed an opponent and the next
    round didn't start" + "voted a map but the next round didn't
    start cleanly." Three structural fixes plus a new game-design
    rule landed together:
    1. **Client never returned to LOBBY after SUMMARY.** ENet
       broadcasts MATCH_STATE phase=LOBBY when the host transitions
       past summary, but `client_handle_match_state` only decoded the
       new state — it didn't update `g->mode`, so the client got
       stuck on the summary screen. Fixed: when phase becomes LOBBY
       and the client isn't already there, run lobby_clear_round_mechs
       and switch to MODE_LOBBY.
    2. **Vote winner clobbered by start_round.** `begin_next_lobby`
       applied the P09 vote winner to `match.map_id`, but
       `start_round` re-ran `config_pick_map(round_counter)` at the
       top, overwriting it. Fixed: only pick from rotation on the
       FIRST round (`round_counter == 0`); subsequent rounds inherit
       map/mode from `after_summary` (which honors the vote).
       Mirrored in shotmode's COUNTDOWN→ACTIVE branch.
    3. **"Only one player remains → 3 s warning → round end" rule.**
       New `match_step_solo_warning` in `match.c`: when the active
       round started with 2+ non-dummy mechs and the alive count
       drops to ≤1 (kill / kick / disconnect), arm a 3-second timer;
       when it expires, end the round. Single-player matches
       (mech_count ≤ 1 from the start) are exempt.
    4. **Multi-round match flow.** A "match" is now N rounds (config
       `rounds_per_match`, default 3, also accepts `match_rounds`).
       Inter-round transitions are seamless: SUMMARY → brief
       COUNTDOWN (`inter_round_countdown_default`, 3 s for prod, 2 s
       for shotmode) → next ACTIVE, with score persisting across
       rounds. NO lobby UI between rounds. After the final round,
       full reset → MODE_LOBBY for fresh ready-up. Implementation:
       - `match.rounds_per_match` / `rounds_played` /
         `inter_round_countdown_default` fields on MatchState
         (host-side; not on the wire).
       - `after_summary(g)` dispatcher in main.c: increments
         `rounds_played`, applies vote winner, branches to
         `end_match` (back to LOBBY, reset stats) or
         `advance_to_next_round` (clear dead mechs, brief countdown).
       - `apply_vote_winner_if_any(g)` factored out so both branches
         honor the vote.
       - Mirrored inline in shotmode's MATCH_PHASE_SUMMARY case.
       - Summary screen UI is phase-aware: SUMMARY shows vote cards
         + "Next round in N s" (or "Match over — back to lobby in
         N s" on the last round); COUNTDOWN hides the cards and
         shows a big "Round N / M starts in N s" banner.
    5. **All-voted fast-forward.** When every active in-use slot has
       a bit set in any of `vote_mask_a/b/c`, `summary_remaining` is
       floored to 1 s so the picker doesn't drag for the full 4–8 s
       window. Lets a 2-player match transition between rounds in
       ~1 s + the inter-round countdown.
  - **New paired test** `tests/shots/net/run_round_loop.sh` (14/14):
    Two clients, `rounds_per_match=2`. Host kill_peers the client in
    each round; solo-warning fires; both vote during SUMMARY; vote
    winner becomes the next-round map; round 1 → round 2 transition
    happens with NO lobby ("client: returning to lobby" only fires
    AT MATCH END, not between rounds); after round 2 the host logs
    "match over" and the client transitions back to LOBBY.
  - **New shotmode directive** `vote_map <choice 0/1/2>` — drives the
    map-vote picker code path. Host-side casts directly via
    `lobby_vote_cast` + rebroadcast; client-side sends
    `NET_MSG_LOBBY_MAP_VOTE`.

  - **Post-P09 self-hitscan-RMB visual fix** (same day): user playtest
    reported RMB-firing Sidearm shows projectiles on the server side
    but not on the firer's own client. Root cause: the skip-self
    guard for HITSCAN in `client_handle_fire_event` blanket-returned
    when `shooter == local_mech_id` on the assumption that the local
    predict path already drew the tracer. But predict only fires on
    `BTN_FIRE` (LMB), never `BTN_FIRE_SECONDARY` (RMB). Same gap
    existed for `WFIRE_MELEE` (predict never draws the swing tracer
    for any path — the comment was just wrong). Fix: predict_drew is
    now `is_self && (FIRE_EVENT's weapon == firer's active slot's
    weapon)` — true only for self LMB-on-active. RMB-on-inactive and
    self melee always render the visual via FIRE_EVENT. Also gates
    the muzzle-spark loop and the projectile-path muzzle tracer on
    `predict_drew` instead of `is_self` so RMB-fired projectile
    secondaries (Frag Grenades, Micro-Rockets, etc.) get full muzzle
    visuals on the firer's screen too.
  - **New paired test** `tests/shots/net/2p_rmb_hitscan.{host,client}.shot`
    + `run_rmb_hitscan.sh` (12 base + 5 RMB-hitscan assertions =
    17/17): both players hold Pulse Rifle + Sidearm. Both RMB-fire.
    Asserts host runs `weapons_fire_hitscan` server-side for both
    mechs (the host's own visual path), and the client's
    `client_fire_event hitscan` SHOT_LOG fires for BOTH `shooter=0
    self=0 active=0` (host's RMB) AND `shooter=1 self=1 active=0`
    (client's own RMB). The pre-fix behavior dropped the latter.

- **P10 — Mech sprite atlas runtime + per-chassis distinctness +
  posture quirks** (2026-05-05).
  - **Bone-length distinctness pass** in `g_chassis[]` (`src/mech.c`).
    Heavy: arm 16→17, torso_h 34→38, neck_h 14→16 (visibly the
    largest). Scout: arm 12→11, forearm 14→13, thigh 16→14, shin
    16→14, torso 26→24 (-16% across the board). Sniper: arm 14→13,
    forearm 17→19, thigh 18→17, shin 18→21, neck 14→16 (long
    forearm/shin, sniper-hunch stance). Engineer: forearm 16→14,
    thigh 18→16, torso 30→32, neck 14→13, hitbox 1.0→0.95 (compact
    stocky build). Trooper unchanged. Bone-length values ripple
    through `mech_create_loadout`'s `DIST(...)` macro (rest lengths
    derived from chassis-relative particle layout) and every
    `ch->bone_*` / `ch->torso_h` / `ch->neck_h` read in `build_pose`,
    so the changes propagate to physics + animation without further
    work.
  - **Per-chassis posture quirks** in `build_pose`: Scout chest
    target offset by `face_dir * 2.0f` x (forward lean), Heavy
    chest pose strength bumped 0.7→0.85 (locked upright), Sniper
    head target offset `face_dir * 2.0f` x + 3 px y down (sniper
    hunch), Engineer skips the right-arm aim drive when
    `active_slot == 1` (tool, not rifle). The factoring introduces
    local `chest_target` / `head_target` Vec2s and a `chest_strength`
    / `skip_right_arm_aim` pair that the chassis switch sets before
    `pose_set` runs.
  - **New module `src/mech_sprites.{c,h}`** (~190 LOC). Public:
    `MechSpritePart` (`src` rect + `pivot` + `draw_w/_h`),
    `MechSpriteId` enum (22 entries — 12 visible body parts + 5
    stump caps + 5 plates/feet/hands), `MechSpriteSet` (`Texture2D
    atlas` + `parts[MSP_COUNT]`). `g_chassis_sprites[CHASSIS_COUNT]`
    is a global mutable table; `mech_sprites_load_all()` populates
    `parts` from a shared Trooper-sized placeholder layout (per
    `documents/m5/08-rendering.md` §"Atlas layout") and tries
    `assets/sprites/<chassis>.png` for each chassis — missing files
    leave `atlas.id == 0` and log INFO. `mech_sprites_unload_all()`
    releases textures.
  - **Replaced `draw_mech` with sprite-or-capsule dispatch** in
    `src/render.c`. Old M4 path preserved as `draw_mech_capsules`
    (no-asset / dev fallback). New `draw_mech_sprites` walks a
    17-entry `g_render_parts[]` z-order table — back-side limbs
    (3 leg + 3 arm) → centerline body (torso + hip plate + 2
    shoulders + head) → front-side limbs (3 leg + 3 arm). When
    `m->facing_left`, `swap_part_lr` and `swap_sprite_lr` flip L↔R
    per entry so the back-side limbs become the right side. Sprite
    rotation: `atan2(b - a) * RAD2DEG - 90.0f` (sprites authored
    vertically, parent end at top of source). Single-particle
    anchors (entries with `part_a == -1`) draw at `part_b` with
    `angle = 0`. Body tint mirrors capsule path
    (alive/dummy/dead/invis) so a mid-development mix renders
    consistent. Held-weapon placeholder line factored into shared
    `draw_held_weapon_line` until P11 lands held-weapon sprites.
    `_Static_assert` locks `MECH_RENDER_PART_COUNT == 17`.
  - **Atlas load path wired** in `main.c` (after `platform_init`)
    and in `shotmode.c` (same site, with `mech_sprites_unload_all()`
    before each `platform_shutdown`). Atlases never load before the
    raylib GL context exists.
  - **New shotmode directive `extra_chassis <name> <x> <y>`** (cap
    6) spawns additional dummy mechs alongside the player + main
    dummy in the legacy world-init path. Lets one shot script
    capture multiple chassis side-by-side.
  - **New shot test `tests/shots/m5_chassis_distinctness.shot`**:
    Player Trooper at x=1060, four extras (Scout 740 / Heavy 900 /
    Sniper 1220 / Engineer 1380) at y=984 on the tutorial map's
    flat floor patch (clear of the slope test bed at x=1500..1987
    and the jet platform at x=384..704). Captures one PNG at tick
    90 after the Verlet solver settles each pose. Heavy is visibly
    the tallest+widest, Scout the shortest, Sniper has the long
    forearm/shin proportions, Engineer is the compact stocky build.
  - **The capsule-rendering trade-off does NOT delete here** — it
    resolves at P12 once damage feedback (hit-flash, decals, stump
    caps, smoke) lands alongside per-chassis atlas art. Per
    `documents/m5/13-controls-and-residuals.md` §"Resolved by M5",
    P10's job is the runtime/data path; P12 lands the user-visible
    capsule replacement.

### Pending

- **P11–P14** — Per-weapon visible art (held-weapon atlas + foregrip
  IK), damage feedback (hit-flash + decals + stump caps + smoke),
  parallax / HUD final art / TTF font / halftone post / decal
  chunking, audio module.
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
