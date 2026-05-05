# TRADE_OFFS — Deliberate compromises and revisit triggers

This document is the *honest* counterpart to the design canon in
[documents/](documents/). The design docs describe what we *want* the
game to be. This file describes what we *settled for* in the current
build, why, and what would have to be true for us to come back and do
it the right way.

Every entry follows the same structure:

- **What we did** — the actual code-level compromise.
- **Why** — what it was solving (and what it cost).
- **Revisit when** — the trigger that should bring this back to the top
  of the queue.

Last updated: **2026-05-04** (post P06).

---

## Physics

### Post-physics kinematic anchor for standing pose

- **What we did** — `mech_post_physics_anchor()` runs after the physics
  step. When a mech is grounded *on a flat surface* (`ny_avg <
  -0.92`, ~22° from vertical) and in `ANIM_STAND` / `ANIM_RUN`, it
  lifts the pelvis + upper body + knees to their standing positions and
  **zeroes Y-velocity** (`prev_y = pos_y`). On slopes, the anchor early-
  outs and the slope-tangent run velocity + slope-aware friction drive
  the pose instead.
- **Why** — Verlet's constraint solver moves position but not `prev`,
  so any non-trivial constraint correction injects velocity. Combined
  with gravity adding velocity each tick, this creates a positive
  feedback loop. We tried softening the pose drive (sag accumulated
  faster than the solver corrected it), hardening the pose drive
  (pumped velocity through constraints), and pinning feet
  (`inv_mass=0`, body explodes upward at run start). The post-physics
  anchor was the only thing that produced rock-solid standing on flat
  ground. P02 added the slope-gating because zeroing Y-velocity on a
  slope kills passive downhill slide.
- **Revisit when** —
  - We add a crouch animation. The anchor's knee-snap will break
    crouch transitions; right now we gate by `ANIM_STAND` and skip
    during run/jet/jump/death.
  - We move to PBD or XPBD (the design doc lists this as a one-week
    refactor). The proper solver doesn't need this hack.
  - We discover the anchor is masking a deeper bug. Symptom would be:
    behavior in run/jump/jet that hints at energy that should have
    been bled off but wasn't.
  - The slope-gating threshold (`-0.92`, ~22° from vertical) feels
    wrong in playtest — body either fails to hold on shallow slopes
    or anchors too aggressively on moderate ones.

### No angle constraints in active use

- **What we did** — `add_angle()` exists in `src/mech.c` and
  `solve_angle()` exists in `src/physics.c`, but no mech currently
  registers any. The solver works (we fixed the π-boundary modulo bug
  by switching to `acos(dot)`).
- **Why** — Angle constraints restrict the *interior* angle at a joint.
  A leg that's been rotated to horizontal still has a π interior angle
  at the knee, so the constraint is satisfied — it doesn't prevent
  the failure mode it was supposed to prevent (lying-down-leg). What
  we'd actually need is an *orientation* constraint relative to a
  world-up reference, which is out of scope for M1.
- **Revisit when** —
  - We need head-not-folding-forward limits on dead bodies. The post-
    physics anchor only runs alive + grounded + standing, so ragdolls
    don't have it. So far ragdoll behaviour looks fine, but a knee
    bending the wrong way on a dramatic kill would warrant the work.
  - We move to PBD/XPBD. A real angle constraint with rest pose and
    stiffness is much easier to write there.

### No torso cross-braces

- **What we did** — Triangulated the torso with `L_SHOULDER↔PELVIS`
  and `R_SHOULDER↔PELVIS` only. Did *not* add `L_SHOULDER↔R_HIP`
  or `R_SHOULDER↔L_HIP`.
- **Why** — Cross-braces oscillate against the shoulder/pelvis
  triangulation pair. Visible as a high-frequency jitter in the chest.
- **Revisit when** — We see torso shear (the shoulders sliding sideways
  off the pelvis under heavy lateral force). Right now the existing
  triangulation handles it.

### Sim runs at 60 Hz, not 120 Hz

- **What we did** — Fixed-step accumulator drives `simulate_step` at a
  hard 60 Hz; the renderer's `alpha = accum / TICK_DT` lerps between
  the start-of-tick particle snapshot (`render_prev_*`) and the
  latest physics result. Vsync-fast displays no longer accelerate
  physics. Per [P03](documents/m5/13-controls-and-residuals.md §B),
  the *rate* stays at 60 Hz because slope-physics tuning happened
  against it; only the accumulator infrastructure landed.
- **Why** — The design doc
  ([03-physics-and-mechs.md](documents/03-physics-and-mechs.md))
  calls for 120 Hz inside a fixed-step accumulator. The accumulator is
  there now; flipping to 120 Hz is `#define SIM_HZ 120` plus a
  re-tune pass on slope friction + jet thrust + air control. Out of
  scope for M5; deferred to playtest after authored maps land.
- **Revisit when** —
  - Playtest reveals 60 Hz feels chunky on jet/jump arcs at modern
    refresh rates.
  - We add bullet tunneling to the table (currently not an issue;
    hitscan + 60 Hz Verlet rarely tunnel at our particle radius).
  - The `tick_hz` config field becomes meaningful (see its entry
    below).

---

## Animation / pose

### Left hand has no pose target (no IK)

- **What we did** — Right hand is driven by the aim vector each tick.
  Left hand has no target — the upper arm and forearm just hang from
  the shoulder.
- **Why** — A two-handed rifle pose needs IK: solve for elbow position
  given hand and shoulder. We don't have a 2-bone IK solver yet, and
  M1's design doc didn't specify one explicitly.
- **Revisit when** —
  - We add weapons that look obviously wrong without a left-hand grip
    (most rifles, every two-handed weapon).
  - Players complain that the mech looks broken.
  - We add a proper animation system (M3 expansion).

### Dummies skip arm pose entirely

- **What we did** — `if (!m->is_dummy)` around the right-arm aim drive
  in `build_pose()`.
- **Why** — A static dummy whose chest sags slightly past its `aim_world`
  point flips its facing direction (right-arm pose target jumps across
  the body), which yanks the constraint solver and pumps lateral force
  into the chest.
- **Revisit when** — Dummies need to aim/track. Right now they're
  punching bags. AI / bots is a stretch goal.

---

## Combat

### Projectile vs bone collision is sample-based, not analytic

- **What we did** — `swept_seg_vs_bone` in `projectile.c` samples 8
  points along the projectile's per-tick motion, finds the closest
  point on the bone for each, and returns a hit if any sample is
  within `(proj_radius + bone_radius)`. Same shape as
  `weapons.c::ray_seg_hit`.
- **Why** — Closed-form swept capsule-vs-segment is a few dozen lines
  of analytic geometry; sampled is 8 lines and we can audit it. At
  60 Hz with 1200-1700 px/s projectiles, per-tick motion is ~20-28 px,
  well over the 6-8 px bone radius — sampling at 8 points covers it
  comfortably. A 1900 px/s microgun slug at the worst case can step
  ~32 px in a tick, edging the 8-sample density; we accept the
  occasional miss and revisit if it's a real complaint.
- **Revisit when** — Players report "I clearly hit them with the
  microgun and nothing happened" patterns. Switch to analytic
  ray-vs-capsule and remove the sample loop.

### No cone bink — just per-shot self-bink + accumulator

- **What we did** — `weapon.bink` (incoming-fire bink) is applied at
  fire-tick to anyone within 80 px of the line origin→end. Projectiles
  apply bink only at the muzzle's near segment, not as the projectile
  travels. `weapon.self_bink` adds aim_bink to the shooter on each
  fire (with random sign).
- **Why** — Per-tick "did this projectile pass within 80px of any
  mech?" is real cost; cheaper to do it once at the muzzle for the
  short-range cone, where most near-misses cluster. Matches the *feel*
  of "their fire makes my aim wobble" without per-projectile
  proximity tests.
- **Revisit when** — Playtesting shows bink doesn't fire on long-range
  projectile sniping. Add a per-projectile-tick proximity check to the
  3 nearest mechs.

### Grapple anchor uses server-current position (no lag comp) (P06)

- **What we did** — When a `PROJ_GRAPPLE_HEAD` lands and
  `projectile_step` decides "tile hit at X" or "bone of mech B at part
  P", it uses the SERVER'S current view of the world. No rewind to the
  shooter's render time. Hitscan weapons go through
  `weapons_fire_hitscan_lag_comp` which rewinds bone history; the
  grapple does not.
- **Why** — Per `documents/m5/05-grapple.md` §"Lag compensation":
  the firer is pulling themselves; the rubber-band correction on a
  missed grapple is sub-100 ms and the visible feedback (rope head
  visibly hitting/missing) is local to the firer. Lag-compensating
  grapple anchors would require a parallel `swept_seg_vs_bone_history`
  + a redo of the constraint allocation against historical positions,
  for a feel improvement that's hard to perceive at LAN latency.
- **Revisit when** —
  - WAN play (50–150 ms RTT) reveals players regularly missing grapples
    they expected to hit on their screen — at which point the bone
    rewind from `weapons_fire_hitscan_lag_comp` becomes the obvious
    next step (refactor to a shared "swept-seg vs. bone at tick T" path).
  - A future patch introduces a "grapple kill" mode (currently
    explicitly zero-damage) where anchor decisions affect combat outcomes;
    the moment grapples can damage, lag-comp matters the way it does
    for hitscan.

### Grapple rope renders as a straight line (P06)

- **What we did** — `render.c::draw_grapple_rope` draws a single
  `DrawLineEx` from R_HAND to the head (FLYING) or anchor (ATTACHED).
  No flex, no sag, no bezier sampling.
- **Why** — Per `documents/m5/05-grapple.md` §"Render": "A flexing-
  rope shader (sampled bezier) is a nice-to-have for v1.5; not v1."
  The straight-line read works visually because the constraint solver
  pulls the firer along the line of the rope; a flex curve would
  contradict the physics' actual line-of-pull anyway. Cosmetic.
- **Revisit when** —
  - Players report the rope looks "wrong" or rigid in playtest,
    especially during ATTACHED while the firer's velocity has lateral
    components (rope should appear to swing).
  - We add catenary sag to anything else (e.g., decorative wires in
    the level format) — at that point the same shader can be reused
    for the grapple.

### Mechs rendered as raw capsules

- **What we did** — `render.c` draws each bone as a thick line and
  particles as small filled circles. No sprite art.
- **Why** — Art pass is M5. Capsules read well enough for movement and
  combat testing.
- **Revisit when** — M5 (maps & content).

---

## World / level

### Hard-coded tutorial map

- **What we did** — `level_build_tutorial()` builds the M1 map in code. P01 shipped the `.lvl` loader; the runtime `map_build` path tries `assets/maps/<short>.lvl` first and falls back to the code-built map if loading fails. Shot mode + the headless harness still use `level_build_tutorial` directly.
- **Why** — P01 shipped the loader/saver but no authored `.lvl` files exist yet (those land at P17/P18) and the editor (P04) hasn't shipped, so there's no way to author a real tutorial map.
- **Revisit when** — P17/P18 ship authored `.lvl` maps. At that point shot tests and the headless harness can load real maps and `level_build_tutorial` can be retired (along with its hardcoded slope test bed — see separate entry).

### Slope-physics tuning numbers are starting values

- **What we did** — P02 ships the friction formula
  `friction = 0.99 - 0.07 * |ny|` (clamped to `[0.92, 0.998]`, ICE→0.998)
  and the post-physics-anchor slope cutoff `ny_avg > -0.92` (~22° from
  vertical). Body lands on slopes via the polygon collision +
  closest-point + push-out path; running projects velocity onto the
  slope tangent.
- **Why** — These are the values the spec doc gives. The English text
  in `documents/m5/03-collision-polygons.md` describes a 60° slope as
  "slide freely" but the formula puts 60° at friction 0.955, which
  combined with the existing pose drive holds the body roughly in
  place rather than producing a dramatic slide. The slope physics is
  wired correctly; the *numeric tuning* needs playtest data we won't
  have until authored maps land.
- **Revisit when** —
  - P17 / P18 ship authored maps with real slope-vocab geometry and a
    bake-test bot that walks slopes; mismatch between intended feel
    and observed behavior triggers tuning.
  - We replace the post-physics anchor with PBD/XPBD (the proper
    solver doesn't fight the slope-tangent run code as hard).

### Renderer draws polygons as flat triangles (P02 stopgap)

- **What we did** — `render.c::draw_level` draws each polygon as a
  filled triangle (color by kind) plus a thin edge outline so shot
  tests see the slope test bed. No texture, no halftone, no parallax
  layering.
- **Why** — P02 needs *some* polygon rendering for visual verification.
  The proper polygon visual lives in P13 (parallax + HUD final art +
  TTF + halftone post + decal chunking) and would be wasted work to
  build twice.
- **Revisit when** — P13 (rendering kit). At that point the stopgap is
  replaced by the sprite-atlas + halftone path; the temporary
  triangle-fill code is deleted.

### Slope test bed is hardcoded in `level_build_tutorial`

- **What we did** — `level.c::level_build_tutorial` allocates 3 SOLID
  polys (45° / 60° / 5° slopes at floor-row mid-map) for shot mode +
  the headless harness to land on. The runtime `map_build` path used
  by real matches doesn't carry these.
- **Why** — Without authored `.lvl` maps (P17), there's no other way
  to get polygons in front of the physics for testing.
- **Revisit when** — P17 / P18 ship authored maps. At that point the
  test bed is removed from `level_build_tutorial` and shot tests use
  the authored `.lvl` files directly.

### Editor undo is whole-tile-grid snapshot for big strokes

- **What we did** — `tools/editor/undo.c` keeps small strokes as
  `(x,y,before,after)` deltas (one entry per painted tile). Bucket
  fills and any other operation that mutates more than a screenful
  of tiles call `undo_snapshot_tiles()` first, which `malloc`s a copy
  of the entire grid. Worst-case undo memory: 64 strokes × 80 KB =
  5 MB on a 200×100 map. Differential per-tile undo is the obvious
  alternative and is rejected on simplicity grounds.
- **Why** — Per-tile delta storage on a bucket fill is O(W·H) and
  needs a custom collapse/coalesce path; the snapshot is a single
  `memcpy` into the editor's permanent arena and undoes via
  `memcpy` back. 5 MB is well under the editor's process budget.
- **Revisit when** —
  - We support map sizes >300×150 (the snapshot grows linearly).
  - A designer reports a perceptible hitch on bucket fill on a slow
    machine (the `memcpy` is fast but not free at very large grids).

### Editor F5 test-play forks a child process

- **What we did** — `tools/editor/play.c` saves the doc to a temp
  `.lvl` and `posix_spawn`s `./soldut --test-play <abs_path>`. The
  editor stays interactive; the game runs in its own window. F5
  always saves to the same fixed temp filename so consecutive
  presses overwrite.
- **Why** — Refactoring `src/main.c` to be re-entrant from the
  editor would require taking apart the `Game` initializer and the
  one-process-per-platform raylib lifecycle. Forking is ~60 LOC
  and lets the game stay exactly as-is.
- **Revisit when** —
  - We add an in-editor "preview the level without leaving the
    editor" mode (real-time scrubbing of changes against a running
    sim). At that point we'd need either an in-process simulate
    loop or some IPC.
  - Cold-start cost of the game binary becomes a designer pain
    point (currently ~1 s on the dev machine).

### Editor file picker is a raygui textbox, not a native dialog

- **What we did** — `tools/editor/files.c` draws an in-app raygui
  modal with a single text-input field and OK/Cancel buttons. The
  editor accepts `argv[1]` as the initial open path; Ctrl+S without
  a known source path or Ctrl+Shift+S opens the modal. No native
  open-file / save-file dialog.
- **Why** — `tinyfiledialogs` is what
  `documents/m5/02-level-editor.md` calls for as the "fourth
  vendored dependency past raylib + ENet + stb." It's ~3 kLOC of
  cross-platform shell-out (zenity / kdialog / osascript /
  GetOpenFileName) that we can replace with a 50-LOC raygui textbox
  for v1. The editor is single-author at v1; a textbox is enough
  to type a path or paste one from the shell.
- **Revisit when** —
  - We hand the editor to a non-engineer designer who has a real
    file dialog on every other tool they use.
  - We add OS-native asset preview thumbnails (a real picker would
    be a natural carrier for that).

### Pickup transient state isn't persisted across host restarts (P05)

- **What we did** — `World.pickups` lives in process memory. A host
  who crashes and restarts mid-round resets every spawner to AVAILABLE
  (level-defined entries) or loses them entirely (engineer-deployed
  transients). Cooldown timers don't survive. State broadcasts on the
  next state transition only — a connecting client gets correct
  state when the next grab happens, but a freshly-restarted host
  shows everyone's "I held the Mass Driver spot" timing reset.
- **Why** — Persistence wants either (a) snapshotting `World.pickups`
  to a local file on every transition (writes per pickup grab, fine),
  (b) sending an INITIAL_PICKUP_STATE bundle on connect, or both.
  Both are real work and we don't have a host-restart use case in M5
  (rounds restart frequently anyway). Spec doc
  (`documents/m5/04-pickups.md`) called this out as acceptable.
- **Revisit when** —
  - Hosts start running long persistent matches where mid-round
    crashes are a real concern.
  - We add an INITIAL_PICKUP_STATE message anyway for mid-round
    join (P05 deferred this; see entry below).

### `NET_MSG_PICKUP_STATE` wire is 20 bytes, not 12 (P05)

- **What we did** — Spec doc 04-pickups.md described a 12-byte
  pickup-state message (msg_type / spawner_id / state / reserved /
  available_at_tick). M5 P05 ships 20 bytes, adding pos_x_q / pos_y_q
  / kind / variant / flags so transient spawners (engineer repair
  packs) can be replicated to clients — without those fields the
  client only knows the spawner's INDEX, not where to draw it or what
  kind of pickup it is.
- **Why** — Engineer repair packs need cross-network visibility (the
  prompt's "Done when" requires "allies can grab it"). Two paths
  considered: (a) extend the existing message to 20 bytes, (b) add a
  separate NET_MSG_PICKUP_SPAWN for transients. We chose (a) — fewer
  message types, simpler dispatch, both branches use the same handler.
  Bandwidth: 20 × ~10 events/min × 16 players ≈ 53 B/s aggregate,
  trivial vs the 5 KB/s/client budget.
- **Revisit when** —
  - We add server-side mid-round-join initial-state shipping (an
    INITIAL_PICKUP_STATE batch). At that point a 12-byte
    state-transition message + a separate spawn message becomes
    cleaner than the unified 20-byte form.
  - We start optimizing wire bandwidth seriously (delta-encoded
    snapshots — see "no snapshot delta encoding" entry).

### Initial pickup state for mid-round joiners not shipped (P05)

- **What we did** — Both host and client call `pickup_init_round`
  on `LOBBY_ROUND_START`, populating their pools identically from
  the level's PICK records. The first state transition (a grab or
  respawn) ships the full 20-byte spawner record. **A client that
  joins after some pickups have already been grabbed sees the wrong
  state** — the host's COOLDOWN entries remain AVAILABLE on the
  joining client until the next transition.
- **Why** — Mid-round join in M4 still parks new connections in the
  lobby until next ROUND_START (we don't spawn mechs mid-round), so
  a fresh joiner never enters MATCH with stale pickup state in
  practice. Shipping a batched INITIAL_PICKUP_STATE in the
  ACCEPT/INITIAL_STATE flow is straightforward (~30 LOC) but adds
  complexity we can defer until matchmaking actually allows
  mid-round join.
- **Revisit when** —
  - Round-already-active mid-round join becomes a real flow.
  - A spectator mode is added (spectators connect mid-round and
    need correct pickup state from tick 0 of their connection).

### `.lvl` v1 format is locked in

- **What we did** — At P01 we shipped the on-disk `.lvl` format
  specified in `documents/m5/01-lvl-format.md`: 64-byte header +
  Quake-WAD-style lump directory + 9 lumps (TILE / POLY / SPWN / PICK
  / DECO / AMBI / FLAG / META / STRT), CRC32 over the whole file
  with the CRC field zeroed. Byte sizes are pinned by `_Static_assert`
  in `world.h` and the wire layout is enforced by explicit `r_u16` /
  `w_u32` etc. helpers in `src/level_io.c`. `LVL_VERSION_CURRENT = 1`.
- **Why** — Once we ship maps in the v1 format (P17 / P18) every
  `.lvl` file we author or hand to a player is locked to the v1
  schema. Bumping the version means: editor save path (P04) needs an
  upgrade pass, level_io needs a v0→v1 migrator, and any in-the-wild
  v1 files load with default-fill for new fields. Forward compat for
  unknown lumps already works (loaders skip unknown names), but
  changing an *existing* record's size is a breaking change.
- **Revisit when** —
  - We discover a fatal flaw in v1 (a record's size is too small to
    hold a field we need; an enum value collides; a designer needs
    something the format can't express). Bump `LVL_VERSION_CURRENT`
    to 2 and write the migrator.
  - We add a feature whose data wants to ride alongside the level —
    e.g., per-mech intro animation positions, scripted events,
    cutscene markers. That data wants its own *new* lump (additive,
    so old loaders skip it), not a v1 record widening.

---

## Networking

### Non-cryptographic handshake token (keyed FNV1a, not HMAC-SHA256)

- **What we did** — `mint_token` in `src/net.c` builds a 32-bit token
  from `fnv1a_64(secret || nonce || addr_host) >> 32` and uses it as
  the CHALLENGE / CHALLENGE_RESPONSE proof. The design canon
  ([05-networking.md](documents/05-networking.md) §"NAT and the IP+port
  join model") calls for HMAC-SHA256.
- **Why** — Adding SHA-256 is ~150 LOC of well-known code we don't
  need to ship M2's "two-laptop test." Threat model is "stranger on the
  LAN" — they can't observe the secret, can't replay because the nonce
  is fresh per-connection, and the address binding makes a token from
  one connection useless on another. A real HMAC-SHA256 buys
  hash-extension resistance we don't currently care about.
- **Revisit when** —
  - We expose the server to the open internet (master server, NAT
    hole-punching). External attackers are a different threat model.
  - Anyone reports anti-spoof bypasses on the LAN.
  - We add encryption-at-rest for chat / lobby messages — at that
    point we'd want a real KDF.

### No snapshot delta encoding (full snapshots only)

- **What we did** — `snapshot_encode` always serializes every mech in
  full (22 bytes each) regardless of what changed. The wire format
  has the `baseline_tick` field but we always set it to 0.
- **Why** — Per [10-performance-budget.md](documents/10-performance-budget.md),
  the budget without delta is **~5.6 Mbps host upstream at 32 players,
  30 Hz**. Above the 2 Mbps target. For M2 ("two laptops shoot each
  other"), uncompressed is ~22 KB/s = 175 kbps each direction —
  trivial. The delta path is the main bandwidth optimization for the
  16+ player case.
- **Revisit when** —
  - We host an 8+ player playtest and a participant reports stutter
    / packet loss correlating with scene complexity.
  - Profiling shows the snapshot path consuming >5% of host CPU.
  - We do an internet-public test where uplink is the bottleneck.

### No server-side entity culling

- **What we did** — Server sends every mech's snapshot to every
  peer regardless of whether the receiver can see it.
- **Why** — Culling cuts bandwidth and is the strongest defense
  against ESP / wallhack cheats (don't send entities the client
  shouldn't know about). Implementing it well needs a per-peer
  visibility model (line-of-sight + small buffer); not load-bearing
  at M2 scale (2 players, both visible to each other).
- **Revisit when** —
  - We ship M3 maps with rooms / vertical layers where two players
    can be off-screen from each other.
  - Bandwidth becomes a problem (see "no delta encoding" above).
  - Cheats become a reported issue.

### Shot-mode driving of UI screens is not built

- **What we did** — `tests/net/run.sh` and `tests/net/run_3p.sh`
  end-to-end the full host+client flow via real ENet loopback and
  assert on log-line milestones. There is no PNG-based shot test
  for the UI screens themselves (title, browser, lobby, summary).
- **Why** — Driving the UI screens through the shot architecture
  requires synthesizing mouse positions / clicks / keypresses
  through raylib (which doesn't have an input-injection API), and
  refactoring `main.c`'s state-machine loop into something
  callable from `shotmode_run`. That's a substantial engine
  refactor for purely-visual coverage. The log-driven network
  tests catch the actual functional bugs (the M4 black-screen-on-
  client regression was found this way), so the marginal value of
  PNG UI tests is mostly visual-regression — easier to do once
  M5's level editor lands and we want the same pipeline for
  authored-content review.
- **Revisit when** — A UI regression slips through the network
  tests AND a screenshot would have caught it. Likely candidates:
  layout overlap at unusual aspect ratios, text-shaping issues
  with non-Latin chars, scaling artifacts at fractional DPI. The
  candidate paths are (a) extend `shotmode.c` with `screen <x>`
  + `click` / `key` directives + a synthesized-input shim that
  the UI helpers consult when in shot mode, or (b) a small
  separate `tools/ui_shots.c` that opens raylib at multiple
  resolutions and renders each screen with hard-coded fake state.

### Default raylib font (no vendored TTF)

- **What we did** — Use `GetFontDefault()` (raylib's 10×10
  pixel-font baked into the library) for all UI text, with
  `SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR)` to
  smooth the upscale and a per-screen UI scale factor (1× at
  720p, snapping in 0.25 increments up to 3× at 4K). Combined
  with `FLAG_MSAA_4X_HINT` for line/shape antialiasing.
- **Why** — A real TTF would be sharper at high DPI but adds an
  asset to vendor (font files are >100 KB; the public-domain
  ones we'd want — Inter, Atkinson Hyperlegible, JetBrains
  Mono — are ~200-400 KB each). The bilinear-filtered default
  font + UI scale is "good enough" for M4's purpose and keeps
  the binary at <3 MB. We can swap to a real TTF in M5 alongside
  the art pass if HUD/lobby text feels mushy.
- **Revisit when** — User-facing complaints about fuzzy text at
  4K, OR a localization push needs glyphs the bitmap doesn't
  cover (any non-Latin script). At that point: vendor a single
  TTF in `assets/fonts/`, load with `LoadFontEx` at, say, 32 px
  base size with codepoint set sized for the language, route all
  `ui_draw_text` calls through it.

### Map vote picker UI is partial

- **What we did** — The protocol carries `vote_map_a/b/c` candidates
  and three uint32 bitmasks for tallies; the server picks a winner
  via `lobby_vote_winner`. But the lobby UI doesn't surface a
  three-card "pick A/B/C" modal at the end of a round — the next
  round just uses `config_pick_map(round_counter+1)` from the
  rotation.
- **Why** — Plumbing is the load-bearing part; the modal is one
  screen of layout work that's easier once we have map preview
  thumbnails (M5 art pass).
- **Revisit when** — M5 maps land with screenshots. At that point
  the summary screen grows a "Vote next map" panel that calls
  `net_client_send_map_vote`; the server tallies and broadcasts
  `LOBBY_VOTE_STATE` (already wired).

### Kick / ban UI not exposed

- **What we did** — `LOBBY_KICK` / `LOBBY_BAN` messages are wired
  end-to-end (`net_client_send_kick/ban`, `lobby_ban_addr`,
  server-side host-only enforcement). The lobby UI doesn't render a
  per-row [Kick] [Ban] button on the player list yet.
- **Why** — Same shape as the vote picker: protocol first, UI
  affordance second. A working kick path also needs `bans.txt`
  persistence to be useful across host restarts.
- **Revisit when** — A host actually wants to moderate, OR M5 ships
  a "host controls" panel. At that point: row hover → buttons,
  confirmation modal, plus `bans.txt` read-on-start /
  write-on-update.

### `bans.txt` not persisted

- **What we did** — `lobby_ban_addr` adds bans to in-memory
  `LobbyState.bans[]`. They survive across rounds in the same
  process but vanish on restart.
- **Why** — File I/O at the right layer is its own can of worms
  (where does it live? what's the format? how do we handle
  concurrent edits?). Ship in-memory, document the gap.
- **Revisit when** — A host actually deals with a problem player and
  asks "did the ban stick?" Add load-on-start / write-on-update of
  a flat `bans.txt` next to the executable.

### `tick_hz` config field accepted but ignored

- **What we did** — `config.c`'s parser silently accepts `tick_hz=`
  if present (well, it logs a warning — there's no key handler for
  it). The simulation runs at fixed 60 Hz from `main.c`.
- **Why** — Aligning the sim rate with the existing 60 Hz vs 120 Hz
  trade-off is one decision per project (see "60 Hz simulation, not
  120 Hz" above). A per-server `tick_hz` would compound that
  trade-off with no upside until we have configurable rates working
  internally.
- **Revisit when** — We finish the fixed-step accumulator (the
  120 Hz refactor); at that point `tick_hz` becomes a meaningful
  knob.

---

## Architecture / process

### Snapshot-style debugging via `headless_sim` is the only test

- **What we did** — `tests/headless_sim.c` runs five scripted phases
  and dumps positions; humans read the output. There's no per-frame
  golden-value assertion, no failure exit code on regression.
- **Why** — Cheap to build, gave us 80% of the value of a real test
  harness. Caught all four major debug rounds during M1.
- **Revisit when** —
  - We get a regression that the dump didn't make obvious. Time to add
    assertions and an exit code.
  - CI starts caring about physics correctness, not just "does it
    build."

### No CI for physics correctness

- **What we did** — `.github/workflows/ci.yml` builds on Linux/Windows
  cross/macOS cross. It does not run `headless_sim`.
- **Why** — Headless test currently produces no exit code on
  regression. Wiring it into CI without that is just running it for
  show.
- **Revisit when** — `headless_sim` gets assertions.

### No data hot-reload (no `src/hotreload.c`)

- **What we did** — Data files (`.lvl`, `soldut.cfg`, future weapon /
  mech tunables) are read at process start or at level load. There is
  no file watcher; in-flight changes need a restart or a level reload.
- **Why** — `documents/01-philosophy.md` Rule 8 calls for a tiny
  mtime-polling watcher (`src/hotreload.c`) so iterating on game-feel
  is fast. We haven't needed it yet — tunables live as `#define`s in C,
  weapon stats live in the `g_weapons[]` table in C, and maps reload
  via the lobby flow's map rotation. The cost of NOT having it is paid
  in iteration time, not in correctness.
- **Revisit when** —
  - The level editor (M5 P04) ships F5 test-play. F5-from-editor wants
    the game to re-read the `.lvl` without a full restart. Either the
    editor talks to a sibling game process (Plan B in
    `documents/m5/02-level-editor.md`) or we add the file watcher.
  - We move weapon / mech tunables out of C into a data file and start
    tuning game-feel from disk.
  - A non-engineer designer starts authoring content and the
    recompile-to-see-change loop becomes the bottleneck.

---

## How to use this file

When you're about to add an "I'll just hack this in for now" — write the
entry here *first*. The discipline of stating "what" + "why" + "when to
revisit" forces you to decide whether it's a real trade-off (worth
keeping) or a code smell that should be fixed before commit.

When a trade-off graduates to a real fix, **delete the entry** rather
than marking it done. This file is a queue of debt, not a changelog.
