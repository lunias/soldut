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

Last updated: **2026-05-03**.

---

## Physics

### Post-physics kinematic anchor for standing pose

- **What we did** — `mech_post_physics_anchor()` runs after the physics
  step. When a mech is grounded and in `ANIM_STAND`, it lifts the
  pelvis + upper body + knees to their standing positions and **zeroes
  Y-velocity** (`prev_y = pos_y`).
- **Why** — Verlet's constraint solver moves position but not `prev`,
  so any non-trivial constraint correction injects velocity. Combined
  with gravity adding velocity each tick, this creates a positive
  feedback loop. We tried softening the pose drive (sag accumulated
  faster than the solver corrected it), hardening the pose drive
  (pumped velocity through constraints), and pinning feet
  (`inv_mass=0`, body explodes upward at run start). The post-physics
  anchor was the only thing that produced rock-solid standing.
- **Revisit when** —
  - We add a crouch animation. The anchor's knee-snap will break
    crouch transitions; right now we gate by `ANIM_STAND` and skip
    during run/jet/jump/death.
  - We move to PBD or XPBD (the design doc lists this as a one-week
    refactor). The proper solver doesn't need this hack.
  - We discover the anchor is masking a deeper bug. Symptom would be:
    behavior in run/jump/jet that hints at energy that should have
    been bled off but wasn't.

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

### 60 Hz simulation, not 120 Hz

- **What we did** — One physics tick per render frame, vsync-bound to
  60 Hz on most displays.
- **Why** — Faster to ship M1. The design doc
  ([03-physics-and-mechs.md](documents/03-physics-and-mechs.md)) calls
  for 120 Hz inside a fixed-step accumulator, with render
  interpolating between the last two simulated states. We didn't build
  the accumulator yet.
- **Revisit when** —
  - Jet/jump arcs feel inconsistent across monitor refresh rates. (We
    have a hint of this already — see "Vsync / frame-pacing leak".)
  - Bullet tunneling at high speeds. (Bullets are hitscan, so this
    isn't a current issue.)
  - First steps of M2 networking — the snapshot rate (30 Hz server) is
    independent of the sim rate, but a 60 Hz sim makes input upload
    less responsive.

### No render-to-sim interpolation alpha

- **What we did** — Render uses the latest sim state directly.
- **Why** — Same as above: not built yet. With 60 Hz sim and render
  capped to vsync, this rarely matters.
- **Revisit when** — We move to fixed-step 120 Hz with a remainder
  alpha. At that point the renderer needs to lerp between the last two
  sim states using `alpha = accumulator / dt`, otherwise we get visible
  judder.

### Vsync / frame-pacing leak

- **What we did** — `simulate()` is called once per render frame.
  When vsync is fast (small window, fullscreen with G-Sync, etc.),
  physics runs faster too — `dt` shrinks but the per-tick caps don't.
- **Why** — Same root cause as the two above: no accumulator.
- **Revisit when** — As soon as we have time, honestly. This is the
  most user-visible of the three deferred-physics-architecture items
  ("jet feels different in fullscreen"). Same fix as 60↔120 Hz: a
  proper fixed-step accumulator.

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

### Burst SMG fires all rounds on the same tick

- **What we did** — `WFIRE_BURST` (Burst SMG) spawns `burst_rounds`
  projectiles in a single call, all on the trigger-press tick. The doc
  describes a 70 ms inter-round cadence.
- **Why** — Proper queuing needs a per-mech "pending burst" state
  (next-round tick + remaining count) and a check in mech_step_drive.
  Acceptable for M3 first pass — the 3-round burst still feels distinct
  from a sustained weapon; the rounds just fan out spatially via
  self-bink rather than temporally.
- **Revisit when** — Playtesting flags the burst as "indistinguishable
  from a single high-damage shot". Add a `burst_pending_*` field on
  Mech and step it down in mech_step_drive; spawn the next round when
  the timer hits zero.

### Grappling Hook is a stub

- **What we did** — `WFIRE_GRAPPLE` registers the cooldown but doesn't
  spawn a projectile or anchor. `mech_try_fire` logs
  `grapple_attempt (NOT YET IMPLEMENTED)`.
- **Why** — A hook needs a projectile head, a "snap anchor on tile or
  bone hit" event, and a per-tick pull (a temporary distance constraint
  with a contracting rest length, plus a release on BTN_USE). About a
  day of work; fits cleanly into projectile.c + a small Hook struct on
  Mech. Punted from M3 to keep the scope on weapons + dismemberment.
- **Revisit when** — A player wants vertical movement in a chassis
  without a strong jet, OR a map design has a "swing across this gap"
  beat. M5 (level editor + maps) is the natural pairing.

### Engineer ability heals self instead of dropping a deployable

- **What we did** — Engineer chassis's BTN_USE adds 50 HP to the
  engineer themselves, on a 30s cooldown.
- **Why** — A "drop a 50-HP repair pack on the ground that allies can
  pick up" needs the pickup system, which lands at M5
  (documents/02-game-design.md §Map pickups). For M3 we wanted the
  ability to *exist* on the chassis so the passive table is real, even
  if its target is the user.
- **Revisit when** — M5 pickup system lands. At that point the
  ability spawns a `PICKUP_REPAIR_PACK` at the engineer's feet with a
  short lifetime; allies (and the engineer) walking onto it consume it.

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


### Mechs rendered as raw capsules

- **What we did** — `render.c` draws each bone as a thick line and
  particles as small filled circles. No sprite art.
- **Why** — Art pass is M5. Capsules read well enough for movement and
  combat testing.
- **Revisit when** — M5 (maps & content).

---

## World / level

### Only a tile grid; no per-tile polygons

- **What we did** — `level.c` builds a 100×40 tile grid; collision is
  particle-vs-tile-rect.
- **Why** — Fast to ship. The design doc
  ([07-level-design.md](documents/07-level-design.md)) calls for tile
  grids with optional polygons per tile. We need polygons for slopes,
  ramps, and one-way platforms; none of those are in the tutorial map.
- **Revisit when** — Map content with slopes (M5), or earlier if we
  want to test a slope-physics feel sooner.

### Hard-coded tutorial map

- **What we did** — `level_build_tutorial()` builds the M1 map in code.
- **Why** — No editor yet (M5).
- **Revisit when** — M5 level editor lands.

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

### No client-side visual smoothing of reconciliation jumps

- **What we did** — `Reconcile.visual_offset` is computed and
  decayed but the renderer never reads it. When the server overrules
  client prediction, the local mech "snaps" to the corrected position.
- **Why** — On a LAN the corrections are sub-pixel; the snap is
  invisible. The smoothing only matters on internet connections with
  sustained 50+ ms RTT. M2 is LAN-only by spec.
- **Revisit when** — We do an internet test (master server / direct
  connect over WAN) and corrections become visible. The hook (visual
  offset + smoothing tick) is already wired; just needs the renderer
  to read it.

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

### Snapshot interp is a 35% lerp, not a full two-snapshot buffer

- **What we did** — `snapshot_apply` translates remote-mech
  particles by 35 % of the (snapshot − current) delta each time a
  snapshot arrives. Over 3-4 snapshots we converge to the server
  position without the per-snapshot pop the user saw on the M4
  initial release. Velocity is set on every particle (not just
  pelvis) so the constraint solver doesn't fight a velocity
  mismatch each tick.
- **Why** — The 05-networking.md design calls for a proper
  two-snapshot buffer with `t = (now − snap_a.time) /
  (snap_b.time − snap_a.time)` lerp, rendering ~100 ms in the
  past. The lerp version is two lines of code and visually
  indistinguishable on LAN; the full interp adds a per-mech
  history ring + a render-time clock + a "what if I lose a
  snapshot" fallback. Defer until we measure the LAN-vs-WAN
  difference in playtest.
- **Revisit when** —
  - WAN testing (RTT > 50 ms) reveals visible drift / lag.
  - We need lag-compensation accuracy that depends on the client
    rendering a known-past timeline (so the server can rewind to
    the SAME past state for hit detection).
  - Big velocity changes happen rarely enough that the lerp's
    convergence-over-3-snapshots looks slow (~100 ms) — e.g. a
    rocket-jump that pings the user across the screen.

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

### Single-player mode has no practice dummy

- **What we did** — Title screen "Single Player" bootstraps an
  offline-solo "server" with one slot (the player). Round runs
  against an empty map; player can move, jet, and shoot at nothing.
- **Why** — M3 had a hard-coded dummy at `(75, 32)` for visible
  feedback in shot tests. M4 removed it from the lobby/match flow
  because a real lobby + auto-spawn would conflict with it. Adding
  a "spawn a dummy slot" path is M5 work alongside the pickup
  system (a "Practice" room could spawn a target dummy as a
  pickup-like entity).
- **Revisit when** — M5 lands the pickup system; add a
  `PICKUP_PRACTICE_DUMMY` (or just a special spawn) for the offline-
  solo path. Until then, shotmode + headless_sim still spawn a
  dummy directly via `mech_create`.

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

### Dummy is a non-dummy on the client

- **What we did** — When the client receives INITIAL_STATE and
  spawns mech 1 (the host's dummy), it spawns it as a regular mech.
  The `is_dummy` bit isn't carried in the EntitySnapshot.
- **Why** — One bit, no room in the current state_bits byte. On the
  client the dummy will try to drive its right arm to its (snapshot-
  set) aim_world, which looks slightly different from the host's
  view but is purely cosmetic — it doesn't move, doesn't fire,
  and the snapshot's pelvis position keeps it where it should be.
- **Revisit when** — We add bots (M3 stretch) or any non-input-driven
  entity. At that point we either widen state_bits to a uint16, add
  a separate flags byte, or remove dummies in favor of bots.

### Loadouts ship per-mech but no lobby UI ~~(M3)~~ — RESOLVED M4

The M3 entry that "the local mech reads its loadout from CLI flags"
is now obsolete. The lobby UI (`src/lobby_ui.c`) cycles loadout
slots; CLI flags pre-fill them as a test escape hatch.

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

---

## How to use this file

When you're about to add an "I'll just hack this in for now" — write the
entry here *first*. The discipline of stating "what" + "why" + "when to
revisit" forces you to decide whether it's a real trade-off (worth
keeping) or a code smell that should be fixed before commit.

When a trade-off graduates to a real fix, **delete the entry** rather
than marking it done. This file is a queue of debt, not a changelog.
