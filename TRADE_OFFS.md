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

Last updated: **2026-05-15** (M6 lobby-loadout-preview). **Added** three
new entries:

- "Loadout preview exhaust anchors at the feet, not at the chassis
  nozzle (M6 lobby-loadout-preview)" — under "Lobby UI". The in-game
  FX path uses `g_chassis_nozzles[chassis_id]` to position each
  jetpack plume (back-mounted for Trooper/Engineer, hip for Sniper,
  pelvis-low for Heavy). That works in real play because the camera
  is wide and the long-tail particle stream extends well past the
  body silhouette. The preview camera is tight (`zoom ≈ 1.3..5.5`
  derived from RT height) and the shorter jets — GLIDE_WING's 36 px
  plume and JUMP_JET's 0 px plume — would otherwise render entirely
  inside the mech sprite and get painted over. The preview therefore
  anchors the visible flame near `(pelvis.x + chassis_x_offset,
  avg(L_FOOT.y, R_FOOT.y) - 4)` and applies a 50 px length floor so
  every jet's COLOUR is unambiguously visible regardless of the
  table value. Real gameplay is unchanged. Revisit when the preview
  modal gains a wider camera (e.g. a portrait/landscape switch) or
  when chassis nozzle positions are no longer "correct" identity
  for a jet type.

- "Spider chart UTILITY axis is a hand-tuned per-secondary table, not
  data-derived (M6 lobby-loadout-preview)" — under "Lobby UI".
  `secondary_utility()` in `src/loadout_preview.c` returns a fixed
  0..1 score per `WeaponId`: Grappling Hook 1.00, Frag Grenades 0.75,
  Micro-Rockets 0.65, Combat Knife 0.55, Burst SMG 0.45, Sidearm
  0.30. Values come from the design intent in
  `documents/04-combat.md` §"Secondary roles", NOT from measured
  fires-per-kill or win-rate data. The other five axes (SPEED /
  ARMOR / POWER / RANGE / MOBILITY) are derived from `g_chassis`,
  `g_weapons`, `g_armors`, `g_jetpacks` so they self-calibrate when
  those tables change. Revisit after M6 P05's bake-test infrastructure
  produces per-loadout match-outcome data; the table should be
  replaced with normalised stats from real games.

- "Loadout preview animation cycle has fixed phase durations, no per-
  player pacing (M6 lobby-loadout-preview)" — under "Lobby UI". The
  16 s cycle (IDLE-primary 2 s, IDLE-secondary 2 s, WALK 2 s,
  SPRINT 2 s, JUMP 1.5 s, JET 3 s, CROUCH 1.5 s, PRONE 2 s) loops
  unconditionally — a player who opens the modal to compare two
  jetpacks side-by-side sees the JET phase only 3 s out of every 16.
  Mitigated by the smart-jump-on-cycle behaviour (clicking the
  jetpack cycle button lands the cycle mid-JET; clicking the
  secondary lands at IDLE-secondary) so most interactive flows DO
  see the relevant phase. Revisit if user feedback shows the cycle
  is too fast (need pause-on-phase) or too slow (skip animations).

Previously: **2026-05-13** (M6 P05 — bot AI improvements). **Added**
four new entries:

- "Bot nav arc-aware JET reach trusts a 800-px-peak two-segment
  approximation (M6 P05)" — under "Bot AI". The bot doesn't simulate
  parabolic physics for reach feasibility; we iterate two-segment ray
  casts from start → high midpoint → end at increasing peak heights.
  Most pillar/wall traversals work but some non-convex polygon shapes
  still false-reject.
- "1v1 PASS rate is 26/32 cells (M6 P05)" — after a cook_maps
  iteration pass (Reactor archway, Catwalk elevated ramps, Crossfire
  shorter stubs, Citadel partition passage) + JET-assist in the bot
  motor, 1v1 60-s bake produces > 0 fires on 26/32 (tier × map)
  cells, up from 11/32 baseline (2.4× improvement). All 8 maps pass
  at Elite + Champion; 6 maps pass at every tier. Remaining 6 cells
  are tier-specific awareness/engagement-picker artifacts, not nav
  topology bugs.
- "Weapon engagement profile numbers are educated guesses, not data-
  derived (M6 P05)" — `g_weapon_profiles[]` ships with hand-picked
  optimal/effective ranges. The plan's "calibrate against measured
  fires-per-kill" workflow needs an iter9/iter10 follow-up sweep with
  the post-P05 AI to find equilibrium values.
- "Bot retreat doesn't actually deny ENGAGE; it competes via score
  multiplier (M6 P05)" — `bot_aggression < 0.30` damps engage score
  by 0.20× rather than zeroing it. If `score_engage * 0.2` still
  beats `score_retreat`, the bot keeps fighting. This is intentional
  for graceful degradation but means Heavy with full ammo + 5 % HP
  may still trade shots with a Light.

**Deleted** "Mass Driver is matrix-dominant; not retuned this pass
(M6 P04)" — Phase 6 retuned to fire_rate_sec 1.25 / aoe_radius 140.
Iter9 matrix shows MD kill total dropped from 76 → 66 (closer to the
55-kill median).

Previously: **2026-05-13** (M6 P04 post-merge polish — lobby bot UX
rewrite + fuel-aware jet). Added three entries: "Bot jet fuel
lockout is a hard 10/40 % hysteresis (M6 P04+)", "Bot map vote is
uniform-random across the 3 cards (M6 P04+)", and
"`NET_MSG_LOBBY_ADD_BOT` is additive, no protocol bump (M6 P04+)".

Previously: **2026-05-13** (M6 P04 — map balance pass + bot AI
hardening). **Added** four new entries: "M5 map geometry deviates
from the original `m5/07-maps.md` design intent (M6 P04)",
"Mass Driver is matrix-dominant; not retuned this pass (M6 P04)",
"Bot opportunistic-fire scan is 1.6× awareness, magic constant
(M6 P04)", and "Riot Cannon is bot-hostile (M6 P04)". No entries
deleted; the bake-test-verdict-is-informational entry from M5 P18
stays relevant.

Previously: **2026-05-12** (M6 P02 — jetpack propulsion FX).
**Added** three new entries under "Jet propulsion FX (M6 P02)":
"Scorch decals are permanent for the round (no per-decal fade)",
"Jet exhaust is client-local (no per-particle wire data)", and
"Heat shimmer uniform loop has a 16-zone hard cap (one per active
mech, at pelvis)". No entries deleted in this round — M6 P02 is
net-additive over M6 P01's earlier shape. **Note**: the audio
fallback path that quietly no-ops on missing files was exercised
during dev (the two new `SFX_JET_IGNITION_*` entries existed in
the manifest for ~5 minutes before the WAVs landed); 49/49 SFX
load post-P02 (47 base + 2 ignition).

Previously: **2026-05-12** (P19 — audio assets fill). **Deleted**
"SFX manifest assets aren't on disk yet (post-P14)" — the 47-entry
SFX manifest + servo loop + 7 per-map music tracks + 7 ambient loops
are all on disk under `assets/sfx/` and `assets/music/`. Sources:
Kenney CC0 audio packs (sci-fi-sounds / impact-sounds / digital-audio
/ ui-audio / interface-sounds / music-jingles) for SFX + servo +
ambient; opengameart.org CC0 for the 7 music tracks. Sourcing
mapping is `tools/audio_inventory/source_map.sh`; format pass is
`tools/audio_normalize/normalize.sh` (22050 Hz mono PCM16 WAV /
mono Vorbis q2-q4 OGG with -16/-23/-28 LUFS loudnorm). Total
shipped: 7.9 MB (well under the 30 MB budget). New Makefile
targets: `audio-inventory` / `audio-normalize` / `audio-credits` /
`test-audio-smoke`. P19 ships **no new TRADE_OFFS entries** — the
multi-variant footstep gap noted in the P14 spec was never wired into
the runtime manifest (single slot per surface with 5-alias rotation),
so P19 ships exactly what the runtime asks for.

Previously: **2026-05-12** (P18 — maps 5-8 + bake harness). P18
extended `tools/cook_maps/cook_maps.c` with builders for Catwalk /
Aurora / Crossfire / Citadel and renamed the existing
"Concourse is synthesized programmatically" entry to cover all 8 M5
maps (P17 + P18). Added the new entry
"Bake-test verdict is informational, not gating (P18)" covering the
new `tools/bake/run_bake.c` harness. Map vote thumbnails ship via a
new `render_thumb` in cook_maps; runtime reads
`assets/maps/<short>_thumb.png` for the vote picker.

Previously: **2026-05-11** (P17 + wan-fixes-16 follow-ups). P17
shipped the first four authored `.lvl` maps via
`tools/cook_maps/cook_maps.c` and added the new entry
"Concourse is synthesized programmatically, not editor-authored
(P17)" plus the amended "Hard-coded tutorial map" + "Slope test bed
is hardcoded in level_build_tutorial" entries (their deletion gate
is the migration of `shotmode` + `headless_sim` callsites off
`level_build_tutorial`, not the existence of authored `.lvl`
files). wan-fixes-16 superseded the wan-fixes-5/-9 dedicated-child
spawn pattern with an in-process server thread after a Windows
spawn-tree UDP bug; the entry at "Host UI's dedicated server runs
in a thread, not a child process" carries the amended history.
See the "2026-05-11 — wan-fixes 1–15" rollup below for the prior
ledger churn (it now covers 1–16 in spirit; the bullet list there
predates the wan-fixes-16 amendment).

Previously: **2026-05-10** (P15 revised — chassis art now comes
from a Soldat-style **gostek part sheet** sliced by
`tools/comfy/extract_gostek.py`, not the AI-diffusion-canonical
pipeline. The diffusion path (skeleton + style anchor → SDXL +
ControlNet-Union + IP-Adapter → crop) plateaued below the art bar:
Illustrious-XL is fundamentally an anime base and IP-Adapter
style-transfer can't pull it into the flat-shaded technical-readout
register the 17-part rigid renderer wants, the spec'd BattleTech LoRA
is SD1.5-incompatible, and a detailed-illustration crop renders as a
same-colour mush at the ~80 px the mech occupies on screen. A gostek
part sheet — one reference image with all 22 body parts laid out
flat-shaded + black-outlined + captioned in reading order — *is* that
register, and the extractor lands it straight into the atlas with a
deterministic crop/rotate/resize/palette-snap pass against
`s_default_parts`. `assets/sprites/trooper.png` now comes from
`tools/comfy/gostek_part_sheets/trooper_gostek_v1.png` via
`extract_gostek.py --palette foundry`. **New entry** "Chassis art is
hand-authored gostek part sheets, not the AI-diffusion pipeline
(P15 revised)" replaces "ControlNet-Union promax + IP-Adapter chain
doesn't fit in 8GB VRAM" and the short-lived "Low-VRAM ComfyUI
fallback workflow" — the AI-diffusion tools stay in `tools/comfy/`
as a documented alternative but are no longer the shipping path. The
"Mech capsule renderer is the no-asset fallback" entry stays open —
only Trooper has a part sheet; Scout/Heavy/Sniper/Engineer + stump
caps land in P16 via the same `extract_gostek.py` path.
`tools/comfy/crop_canonical.py` (now shared infra between the gostek
and diffusion paths) lost the baked Bayer screen from `post_process_tile`
— the runtime `halftone_post` shader screens at framebuffer res, so
baking a Bayer pattern into the sprite double-dithered it; it now does
a flat 2-colour luminance snap and gained a `rotate` crop-table column.

Previously: 2026-05-10 (P15-revisit — Trooper chassis atlas
regenerated through the *full* Pipeline 1 on an RTX 5090 / 32 GB:
Illustrious-XL v0.1 base + ControlNet-Union SDXL 1.0 promax + IP-Adapter
Plus SDXL ViT-H + KSampler dpmpp_2m_sde/karras + sdxl-vae-fp16-fix on
GPU; superseded by the gostek pivot above. The "ControlNet-Union promax
+ IP-Adapter chain doesn't fit in 8GB VRAM" entry was deleted then; the
8 GB fallback workflow JSON stays on disk as part of the now-secondary
diffusion path.).

Previously: 2026-05-10 (post-P15 — first chassis atlas on disk +
two render-side sprite-anchor bug fixes: `draw_mech_sprites` scales
sprite dst_h to match the actual bone span; foot pivot moved to the
bottom edge so the boot draws above the FOOT particle. First Trooper
atlas was generated on an RTX 2080 8GB / WSL2 via an 8GB-tuned
subset of Pipeline 1 — superseded.).

Previously: 2026-05-09 (post-P14 — audio module shipped:
`src/audio.{c,h}` with 47-entry SFX manifest + alias pool +
listener-relative pan/volume; 5 software buses with default mix
1.00/1.00/0.30/0.45/0.70 for MASTER/SFX/MUSIC/AMBIENT/UI; ducking on
big detonations (`audio_request_duck(0.5, 0.30)` from
`explosion_spawn` for AOE damage ≥100); per-map music streaming via
`audio_set_music_for_map`; ambient loops via `audio_set_ambient_loop`;
servo loop modulated by local-mech velocity; ~30 call sites wired
across `weapons.c` (fire SFX + hit SFX), `projectile.c` (explosion
buckets + grapple hit), `mech.c` (footsteps via gait wrap, jet pulse
rate-limited, grapple fire/release, kill fanfare + death grunt),
`pickup.c` (per-kind grab + high-tier respawn), `ctf.c` (flag SFX
on host) + `client_handle_flag_state` (mirrored on client),
`ui.c` (click + toggle), `client_handle_fire_event` (mirrored fire
SFX gated on `!predict_drew`), `client_handle_hit_event` (HIT_FLESH).
Hot-reload registers all 47 SFX paths + servo path via
`audio_register_hotreload` (DEV_BUILD-gated, mtime watcher). Per-map
music switch resolved through `apply_audio_for_map` reading
`level.meta.music_str_idx` + `ambient_loop_str_idx` from the level's
STRT lump. **New entries**: SFX manifest assets aren't on disk yet
(P15+); No crossfade between map tracks; No 3D HRTF / binaural pan;
Servo loop only for the local mech; Footstep SFX is concrete-surface
only at v1; Held-jet pulse rate-limit picked by integer tick count;
Client-side mech_kill audio is server-only; UI hover sound not wired.

Previously: 2026-05-09 (post-P13 — rendering kit shipped: TTF
fonts (Atkinson + VG5000 + Steps Mono) wired through `platform_init`
with graceful default-font fallback; halftone post-process shader +
backbuffer-sized RT; new `src/map_kit.{c,h}` for per-map parallax +
tile atlas with no-asset fallbacks; 3-layer parallax draw; tile-sprite
rendering with 2-tone fallback; free polygon refactor (`draw_polys` +
`draw_polys_background`); decoration sprites with hash-based sub-rect
lookup + 4-layer dispatch + ADDITIVE/FLIPPED_X flag handling; HUD
atlas-aware bars + crosshair + kill-feed icon variants + `DrawTextEx`
across the HUD; `src/decal.c` chunked into 1024×1024 lazy-allocated
tiles when level >4096 px; new `src/hotreload.{c,h}` DEV_BUILD-gated
mtime watcher with reloads for chassis / weapons / decorations / HUD /
halftone shader. **Resolved**: `Default raylib font (no vendored TTF)`
deleted. **New entries**: per-map kit textures + decoration sub-rect
hash + decoration placeholder rectangles + halftone density hard-coded
+ HUD atlas not on disk + decal-layer chunk lazy-alloc soft cap +
decal dirty-bit reserved + hot-reload registration is a fixed startup
set. **Updated**: `Mech capsule renderer is the no-asset fallback`
note unchanged — the capsule path stays the canonical render until
P15/P16 ships authored chassis atlases.

Previously: 2026-05-09 (post P12 — damage feedback layers shipped.
New entries: "Whole-mech hit flash, not per-particle" (per-particle
flash would need a separate hit-flash field per particle in the SoA
pool; whole-mech is sufficient at v1), "Decal-overlay damage, not
sprite-swap damage states" (the canonical compromise from
`documents/m5/12-rigging-and-damage.md` — 3× asset reduction, 5
shared decal sub-rects total), "Decal records use sprite-local int8
coords" (±127 px range fits all current sprites; widen to int16 if a
chassis goes bigger), "Symmetric mech parts to avoid flip-past-180°
artifact" (no `flipY` in the sprite path; asymmetric parts get
per-part flipY in M6 polish), "No re-sort by Y mid-tumble" (dead-
body limb cross-overs read briefly weird; tolerated). Updated the
"Mech capsule renderer is the no-asset fallback" entry's note —
sprite path is now canonical (P12 ships the damage feedback in both
paths) but `assets/sprites/<chassis>.png` doesn't ship until
P15/P16, so capsule fallback is what fires in real play through
P14 development.

Previously: 2026-05-09 post-P11 — per-weapon visible art runtime
+ post-ship foregrip-pose revert. Foregrip pose was attempted (three
variants: strength-0.6 L_HAND yank, clamped L_HAND, snap-pose-IK
strength-1.0 on L_ELBOW+L_HAND); all three drifted the mech body in
the aim direction during steady-state hold. Reverted; "Left hand has
no pose target" trade-off updated to cover both one-handed and
two-handed weapons until a real 2-bone IK constraint lands inside
the solver loop. Added "Held-weapon line fallback when weapons
atlas is missing" entry — P11 ships the runtime + sprite-def table;
`assets/sprites/weapons.png` arrives at P16. New "Muzzle flash is a
render-tick disc, not a pooled FX particle" entry (cosmetic stopgap
until P13). Migrated fire-path muzzle origin from
`Weapon.muzzle_offset` (single float) to `weapon_muzzle_world(...)`
so visible muzzle and physics muzzle coincide; the float field stays
as the fallback. P10 audit (refreshed the capsule trade-off) carried
over.

Previously: 2026-05-05 (post P09 — controls + host-controls panel
+ `bans.txt` persistence + 3-card vote picker + multi-round match flow.
Resolved three M4-era entries: "Map vote picker UI is partial",
"Kick / ban UI not exposed", "`bans.txt` not persisted". New
post-P09 entries below cover the placeholder map-vote thumbnails and
the ban-by-name simplification.).

2026-05-11 — **wan-fixes 1–15** ship between P16 and P17 as off-roadmap
WAN polish. Net effect on the trade-off ledger:

- **Deleted** "Single-byte (7-byte) kill event drops headshot/gib/overkill flags"
  (wan-fixes-13 widens to 39 bytes + populates client `world.killfeed[]`).
- **Deleted** "Loadout doesn't survive client process restart" (wan-fixes-8
  ships `soldut-prefs.cfg`).
- **Amended** "Remote mechs render in rest pose on the client" entry to
  mention wan-fixes-4's `SNAP_STATE_RUNNING` wire bit + gait-lift
  hysteresis (the anim_id still rides the wire; bone-state remains
  rest pose with kinematic rigid-translate per wan-fixes-3).
  **M6** then retired this entry entirely: the procedural pose
  function in `src/mech_ik.c` now drives bones for every mech on
  every client, replacing it with the new "Live-mech bones are
  procedural; Verlet is for ragdoll only" entry below.
- **Deleted** (M6) "Left hand has no pose target (no IK)" — the
  procedural pose function solves a 2-bone analytic IK for the
  off-hand toward the weapon's `WeaponSpriteDef.pivot_foregrip`
  when present, with no constraint-solver feedback into PELVIS
  because the pose is the LAST writer per tick.
- **New entries** added below for: AOE explosion ~RTT/2 visual gap,
  kill-feed 16-byte name truncation, host architecture forces a
  dedicated child even for offline-solo, host-setup overlay can't be
  cancelled mid-spawn, prefs file is per-cwd not per-user.

---

## Physics

### PRONE drifts ~0.7 px/tick along facing (M6)

- **What we did** — `ANIM_PRONE` rotates the entire standing skeleton
  90° around the pelvis so every inter-bone offset preserves its
  rest-length. Cross-brace constraints (`PELVIS-L_SHOULDER`,
  `PELVIS-R_SHOULDER`, etc.) stay at rest, so the solver doesn't
  fight the rotation the way a literal "lower torso" pose did.
- **Why** — The skeleton's distance constraints were authored for a
  standing body. A rotated pose preserves them perfectly. A
  "compressed-but-upright" prone pose (low pelvis, shoulders pulled
  in) violated several rest lengths and produced multi-px-per-tick
  drift. The 90° rotation is the geometric trick that lets us
  reuse the standing constraint graph for a different orientation.
- **Cost** — There is still a residual ~0.7 px/tick (~42 px/sec)
  forward drift while the player holds PRONE. Cause: friction +
  iterative constraint solve interact asymmetrically with the
  rotated foot-on-ground contact. Single-tick correction errors
  accumulate. Magnitude is small enough that a quick prone is
  effectively stationary, but a held prone visibly creeps forward.
- **Revisit when** —
  - A player wants to hold prone for long stretches (e.g. a sniper
    overwatch role). The drift becomes more obvious.
  - We add proper skeletal animation (M3 expansion). A baked prone
    frame doesn't drift.
  - We rewrite the constraint solver to use position-only PBD (no
    velocity injection through constraint corrections), which would
    eliminate the friction-vs-constraint asymmetry that produces
    the drift.

### Remote-mech bones need a post-pose terrain push (M6)

- **What we did** — Added `physics_push_mech_out_of_terrain` which
  runs AFTER `pose_write_to_particles` for every alive mech. Unlike
  `physics_constrain_and_collide`'s tile / poly collision passes
  (which gate on `inv_mass > 0` per the wan-fixes-3 kinematic-remote
  trade-off), this one-pass push ignores `inv_mass` so remote mechs
  on the client get the same surface clipping the local mech does.
  Kinematic translate (pos AND prev shifted by the same delta)
  preserves velocity.
- **Why** — The procedural pose function writes deterministic bone
  offsets from the pelvis. On a slope, the straight-down feet
  target lands INSIDE the slope poly. For the LOCAL mech, physics
  had already pushed bones out of the slope earlier in the tick,
  and `pose_write`'s swept-test clipped the move at the slope
  surface. For the REMOTE mech viewed from another client (where
  `inv_mass=0`), physics did nothing, and `pose_write`'s swept-test
  saw both endpoints inside the slope (so `level_ray_hits` returned
  false — ray-cast convention is "starting inside doesn't count").
  The bones ended up inside the slope, rendering as a rigid
  straight-legged pose vs. the local view's slope-adapted pose.
- **Cost** — Per-mech, per-tick. For every alive mech (auth and
  client side) we redo a 16-particle × ~3×3-tile-neighborhood
  collision check. Negligible. The function is idempotent on
  already-outside-terrain particles (the inside checks return
  immediately).
- **Revisit when** —
  - A new constraint kind imparts pos changes outside the regular
    `physics_constrain_and_collide` loop and that need a terrain
    safety net. Today it's the grapple+pose pair; we can keep
    adding callers without a real refactor.
  - We move to a single-pass simulation where pose and constraint
    relaxation interleave; then the post-pose push becomes
    redundant.

### Grapple rope is soft (10% stiffness per relaxation iter) (M6)

- **What we did** — `solve_fixed_anchor` (tile anchor) and
  `solve_distance_limit` (mech-bone anchor) apply only
  `GRAPPLE_ROPE_STIFFNESS = 0.10` of the overshoot per iteration.
  With the 12-iter relaxation loop that yields ~72% per-tick
  correction (1 - 0.9^12), and the constraint move is ALSO
  swept-tested against solid tiles — if pulling the firer toward
  the anchor would drag them through a wall, the move clamps at the
  wall surface.
- **Why** — Pre-fix the rope applied 100% of the overshoot per iter
  AND skipped the swept-test, which meant: (a) the firer snapped to
  rope length on the first relaxation iter (no stretchy feel — felt
  like a steel cable, not a rope); (b) during retract (rope length
  shrinking 13 px/tick at the old 800 px/s rate) the firer's pelvis
  was dragged straight through any geometry between it and the
  anchor. User play-test repro on the first M6 P01 build. Halving
  the retract rate to 400 px/s + adding stiffness + adding the
  swept-test together produced the "Tarzan rope" feel: visible
  stretch on attach, smooth pendulum swing, retract pulls firer up
  AROUND obstacles instead of through them.
- **Revisit when** —
  - A skill weapon (e.g. a tow-cable or harpoon) wants a HARDER
    rope. Add a `stiffness` field to `Constraint` so the same solver
    can serve both.
  - Cross-platform play introduces float-reorder drift in the
    constraint pull such that two clients see the firer at noticeably
    different rope positions. Currently both sides run the same
    PBD-relaxation deterministic math so they converge to the same
    pose; if that breaks, we'd need either a fixed-point constraint
    solver or a snapshot bone broadcast on every fire.
  - The constraint-solver's "swept-test stops at the wall" produces
    a visible hitch when the firer should swing AROUND a corner
    (true rope wrapping). True wrap-around requires tracking the
    rope's path through corners — complex; deferred.

### Live-mech bones are procedural; Verlet is for ragdoll only (M6)

- **What we did** — Live (alive=true) mech bone positions are produced
  each tick by `pose_compute` in `src/mech_ik.c`, run after physics
  and `mech_post_physics_anchor` inside `simulate_step`.
  `pose_write_to_particles` writes both `pos` AND `prev` for every
  non-dismembered particle, so Verlet sees zero injected velocity.
  Bone shape is a pure function of `(pelvis, aim, facing, anim_id,
  gait_phase, grounded, chassis, active_slot, dismember_mask,
  foregrip_world, grapple_state)`. The 12-iter constraint solver
  still RUNS each tick, but its output is overwritten by pose for
  live skeletons — the solver's only meaningful work is for dead
  ragdolls (alive=false; pose skips them) and dismembered free-flying
  limbs (per-particle skip in `pose_write_to_particles`).
- **Why** — Replaces M5's `build_pose` + `apply_pose_to_particles`
  driver, which was history-dependent (each tick's bone positions
  were a function of the previous tick's bones + nudge factor +
  12 relaxation iterations). Two clients running the same input
  stream could drift over time; remote mechs froze in rest pose
  (wan-fixes-3) to avoid drift on kinematic particles. Procedural
  pose retires both problems: bones are pure-function output of
  synced state, so every client renders the same skeleton. Gait
  phase rides the wire (`gait_phase_q` u16, +2 bytes/mech/snapshot)
  so every viewer renders the same foot frame. The `inv_mass=0`
  remote-mech kinematic gate from wan-fixes-3 stays — physics-driven
  motion on remote mechs would just be discarded by pose anyway, and
  the gate prevents Verlet feedback into bones the snapshot stream
  is responsible for.
- **Cost** —
  - Design doc 03-physics-and-mechs.md describes Verlet as the
    authoritative driver. The reality is that pose overrides physics
    for live skeletons every tick — Verlet contributes only the
    pelvis motion (gravity, run, jet) for the local mech, and the
    constraint solver is effectively dead code for live mechs.
  - Each live mech writes the full 16-particle skeleton kinematically
    each tick, even though physics already moved some of them — the
    physics work is wasted for non-pelvis bones. ~16 mechs × 15
    redundant writes/tick ≈ 240 writes/tick. Negligible.
  - SHOT_LOG `inside_tile` events fire each tick where the
    deterministic foot placement lands on the floor surface. Floor
    surface y == foot_pose_y exactly, so the discrete-iteration
    constraint solver flags it as inside before the per-tick pose
    write resets the bones. Diagnostic spam only; production play
    (no SHOT_LOG) is unaffected.
- **Revisit when** —
  - A bone position read needs to be physics-accurate (e.g. a damage
    decal hit-test that compares against the live constraint-solver
    state). Currently decals read post-pose bones, which match what
    render draws.
  - Cross-platform play introduces float-reorder drift past the
    1 px tolerance (current target). Would force a fixed-point pose
    function or a "bone broadcast every N seconds" sync.
  - Authored skeletal animation lands (M3 expansion). Procedural
    rules give way to per-anim Frame[].Pos[] tables like Soldat's
    `.poa` files; the dispatcher stays in `mech_ik.c`.

### Footstep SFX fires from two paths (M6)

- **What we did** — Authoritative side (server + offline + client's
  local mech) fires `SFX_FOOTSTEP_CONCRETE` from
  `mech_update_gait` in `src/mech.c` when `gait_phase_l` wraps
  from >0.5 → <0.5. For REMOTE mechs on the client,
  `mech_update_gait` is gated off (snapshot ships the
  authoritative gait_phase), so the wrap detection lives in
  `snapshot_apply` in `src/snapshot.c` and fires the same SFX
  using the same plant-location math.
- **Why** — The pose function is pure (no side effects) — it
  computes bones from inputs. Footstep SFX is a side effect of
  the gait phase advancing past a threshold, which is a fact
  the receiver of the gait_phase update has to observe, not the
  pose function itself. Splitting between the two update paths
  keeps `pose_compute` testable in isolation and lets the SFX
  fire on the client without a separate wire message.
- **Revisit when** —
  - The duplicated wrap-detect logic drifts (e.g. one path fires
    on the wrong threshold). Today the two implementations match
    by construction; a unified helper would centralize it.
  - We add per-surface footstep variants (metal / ice / wood).
    The surface lookup would live in one helper called from both
    paths.

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
    crouch transitions; we currently snap knees only in `ANIM_STAND`
    (`ANIM_RUN` lets the stride drive knee X swing), so a crouch
    state needs the same carve-out. The anchor itself runs in both
    `ANIM_STAND` and `ANIM_RUN`; JET/FALL/DEATH skip naturally via
    `grounded == false` or `alive == false`.
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

### Held-weapon line fallback when weapons atlas is missing (post-P11)

- **What we did** — P11 shipped `src/weapon_sprites.{c,h}`: 14-entry
  `g_weapon_sprites[]` table (grip / foregrip / muzzle pivots in
  source-rect-relative px) + `g_weapons_atlas` shared `Texture2D` that
  loads from `assets/sprites/weapons.png`. When the atlas isn't
  present (which is true at P11 ship — the file arrives at P16 with
  the asset-generation pipeline), `draw_held_weapon` in
  `src/render.c` falls back to a per-weapon-sized line (`draw_w *
  0.7`, clamped against solids via the existing `draw_bone_clamped`).
  A Mass Driver still reads visibly longer than a Sidearm, but the
  line silhouette doesn't carry the visible identity the spec
  describes.
- **Why** — Splitting the runtime (P11) from the asset generation
  (P16) lets each land as a focused review surface; the atlas
  pipeline is the substantial work in P15/P16. Shipping the sprite-
  def table + draw path with a graceful fallback means the sprite
  path is ready the moment the PNG drops in — no further plumbing.
  Per-weapon line distinctness keeps shot tests informative during
  development.
- **Revisit when** —
  - P16 ships `assets/sprites/weapons.png`. At that point the atlas
    branch fires and the fallback becomes dead code in real play; the
    fallback can stay as a dev-machine convenience until any future
    decision to drop it.
  - The `draw_w * 0.7` heuristic stops feeling right at the edges
    (Sidearm's 20-px line is barely visible; Mass Driver's 67-px line
    can clip the body silhouette). Tune the multiplier or drop the
    fallback entirely.

### Muzzle flash is a render-tick disc, not a pooled FX particle (P11)

- **What we did** — `draw_held_weapon` checks `m->last_fired_tick`
  and, when within 3 ticks of the most recent fire, emits a single
  `DrawCircleV` call at the visible muzzle in `BLEND_ADDITIVE` mode
  (yellow-orange, alpha-tapered over the window). Bypasses the FX
  pool entirely.
- **Why** — At P11 the muzzle flash is a feedback layer, not a
  reusable visual. Pooling it would require a new `FxKind`
  (FX_MUZZLE_FLASH) plus `fx_spawn_*` plumbing for ~3 ticks of
  visibility per shot. The render-side disc costs a single branch +
  draw call per mech per frame; the pool would cost the same per
  flash plus the per-tick fx_update walk.
- **Revisit when** —
  - P13 ships per-weapon-class muzzle-flash sprites (the spec at
    `documents/m5/12-rigging-and-damage.md` §"Render path" + the
    HUD/atlas pass) and the disc gets replaced with a real sprite
    drawn from a flash atlas.
  - We need flashes to persist across the firer's death (e.g. a
    final-frame killcam shot where the muzzle flash should still be
    visible after the body ragdolls). The render-tick form requires
    the firer's mech alive to read; pooled FX would survive.

### Chassis art is hand-authored gostek part sheets, not the AI-diffusion pipeline (P15 revised)

- **What we did** — `assets/sprites/<chassis>.png` is produced by
  `tools/comfy/extract_gostek.py`: a "gostek part sheet" — one
  reference image (Trooper's is 2048×2048) with all 22 body parts laid
  out flat-shaded, black-outlined, and captioned in reading order
  (the layout Soldat's gostek model uses: per-part rigid skinning,
  each sprite hooked to a skeleton point — see
  `wiki.soldat.pl/index.php/Mod.ini` and OpenSoldat's
  `client/GostekRendering.pas`) — gets sliced (scipy connected-component
  detection of the 22 shapes → band-cluster into reading rows →
  caption-sequence → `s_default_parts` slot) and each part is
  rotated/flipped to its vertical/horizontal atlas slot, resized to the
  runtime sprite size, white-keyed (border flood so enclosed white
  panels survive), and optionally palette-snapped to the map's two
  colours. The output composites at the same dst rects as
  `src/mech_sprites.c::s_default_parts`, so no transcribe pass is
  needed. The AI-diffusion-canonical pipeline (`asset.py` orchestrator
  + `mech_chassis_canonical_v1.json` / `_8gb_v1.json` workflows +
  `crop_canonical.py` canonical-crop mode + `make_trooper_skeleton.py`
  / `prepare_skeleton.py` skeletons + `build_crop_table.py`) stays in
  `tools/comfy/` as a documented alternative path but is **not** what
  the build ships from.
- **Why** — The diffusion path plateaued below the art bar after four
  parameter/prompt iterations (committed config → tuned knobs →
  +lineart LoRA → +Canny preproc + LoRA @1.0 + technical-readout
  prompt): Illustrious-XL is fundamentally an anime base, IP-Adapter
  *style transfer* re-weights but can't override that prior, and the
  one BattleTech-Battlemechs LoRA the art-direction doc names is
  SD1.5-architecture and won't load on SDXL. Even when the canonical
  *looked* acceptable at 1024², cropping it into 17 detailed-illustration
  tiles produced same-colour orange mush at the ~80 px the mech occupies
  on screen — the rigid 17-part renderer wants clean flat plates with
  hard outlines, which is exactly what a gostek part sheet is. So the
  pivot isn't a stopgap — it's choosing the input format that matches
  the renderer. The cost: the chassis sheets are authored (by hand, or
  generated out-of-repo) and committed as PNGs rather than being
  reproducible from a prompt + seed in this repo; `assets/credits.txt`
  + `documents/art_log.md` carry the provenance per the iteration-log
  convention.
- **Revisit when** —
  - A Soldut-trained LoRA (per `documents/m5/11-art-direction.md`
    §"Escalation path 1", once ~30+ approved sprites exist) or a
    different base model closes the quality gap *at the renderer's
    on-screen size* — at which point the diffusion path could feed the
    gostek-extraction step (generate the part-sheet layout with AI,
    still slice it deterministically) and this entry narrows to "the
    gostek sheet is now AI-generated".
  - A contributor produces a higher-quality hand-drawn or vector
    gostek sheet for any chassis — drop it in `gostek_part_sheets/`,
    re-run `extract_gostek.py`, update `art_log.md` + `credits.txt`.
  - We decide the diffusion tooling has bit-rotted and nobody uses it —
    delete `asset.py` + the canonical workflows + `crop_canonical.py`'s
    canonical-crop mode and fold this entry's "alternative path" clause
    away.

### Whole-mech hit flash, not per-particle (P12)

- **What we did** — `Mech.hit_flash_timer` is set to 0.10 s on every
  successful damage application in `mech_apply_damage`. The renderer
  reads `timer / 0.10f` as a 0..1 white-additive blend over the body
  tint — applied once per Mech, modulating every limb sprite (or
  capsule color) uniformly. No per-particle / per-limb granularity.
- **Why** — Per-particle flash would need a separate hit-flash field
  per particle in the SoA pool (~12 KB across all mechs at peak).
  Whole-mech flash is the right granularity for v1: the player gets
  a clear "hit landed" cue without needing to identify which arm took
  the bullet (the kill feed already attributes the damage). The cost
  is one float per Mech and one branch per render call.
- **Revisit when** —
  - Playtest reveals players genuinely can't tell which limb got hit
    when watching a hit-flash ("which arm did I just shoot?"). At
    that point per-particle flash on the SoA pool is the natural
    extension — the parallel array exists already.
  - We add a vulnerable-spot mechanic where hitting a specific part
    matters tactically (e.g. a "core" particle that takes 2× damage).
    Per-particle flash communicates that.

### Decal-overlay damage, not sprite-swap damage states (P12)

- **What we did** — Damage events drop a decal record onto the hit
  limb's `MechLimbDecals` ring (`(local_x, local_y, kind)` per entry,
  16-deep, oldest-overwrite). The renderer composites those decals
  on top of the limb sprite each frame using the bone segment's
  current angle. We do NOT ship `pristine.png` / `damaged.png` /
  `heavy.png` per-part variants.
- **Why** — Three asset variants per limb × 17 visible parts × 5
  chassis = 255 sprites just for damage states. The decal-overlay
  approach uses 5 shared sub-rects total (3 dent variants + 2 scorch
  per spec; 3 placeholder colored circles at P12 ship until P13's
  HUD atlas drops them in). 50× asset reduction with comparable
  visual feedback. Decal records are i8 sprite-local px so they
  migrate with the bone naturally; the same sprite continues to
  render with a decal "stuck" to it as the body moves.
- **Revisit when** —
  - The decal-overlay reads visually wrong against authored chassis
    art (e.g., a decal sized for one chassis's plate looks weird on
    a Heavy's larger pauldron). At that point either: (a) author
    per-chassis decal variants in the HUD atlas, or (b) scale decal
    sub-rects by the part's `draw_w / draw_h` at composite time.
  - We want truly distinct damage stages (e.g. "exposed wiring at
    50% HP", "shattered armor at 25%") rather than just accumulated
    pock-marks. A per-limb damage-stage swap would be a real visual
    upgrade — but it doubles or triples the asset count and we'd
    only do it once we know the base art is shipping.

### Decal records use sprite-local int8 coords (P12)

- **What we did** — `MechDamageDecal.local_x` and `local_y` are
  `int8_t` (-127..127). The decal-record path in `mech_apply_damage`
  clamps both axes to that range before storing.
- **Why** — Sprite-midpoint-relative coords for our largest sprite
  (Trooper leg-upper at 36×96 px) span ±48 px, comfortably inside i8
  range. i8 cuts decal storage to 4 bytes per entry (vs 8 for two
  i16 / two float). 16 entries × 22 visible parts × 32 mechs = ~45 KB
  total, vs ~90 KB at i16.
- **Revisit when** —
  - A future chassis goes wider than 254 px on either axis (none of
    the five M5 chassis come close). Widen to int16 — the
    pixel-coords-not-fraction discipline survives the change.
  - We add stretchable / scaling sprites where a decal could land
    far from the midpoint. Same fix.

### Symmetric mech parts to avoid flip-past-180° artifact (P12-relevant)

- **What we did** — `draw_mech_sprites` rotates each limb sprite by
  `atan2(b - a) * RAD2DEG - 90.0f`. When a bone rotates past straight
  up, raylib's rotation crosses the ±π boundary cleanly but the
  sprite's "up" axis flips relative to the body. We don't apply
  per-frame `flipY` correction; instead, we ship the chassis art
  with parts drawn as top-to-bottom symmetric (so a flipped V
  coordinate is invisible).
- **Why** — Soldat's `Render.pas` applies a per-part flipY when the
  bone's parent-relative angle crosses ±π/2 — ~15 LOC of bookkeeping.
  Symmetric authoring is cheaper at our atlas size (5 chassis × 22
  parts) and keeps the renderer flat. The asymmetric parts in our
  spec (Sniper helmet visor, Engineer shoulder-mounted welder) are
  small enough to author with a "rotates correctly within the
  expected pose range" check.
- **Revisit when** —
  - A new asymmetric part causes visible weirdness during ragdoll
    tumbles (the head's visor rotates upside-down when the body
    cartwheels). At that point: add per-part flipY to the few
    asymmetric sprites, not blanket. ~15 LOC in `draw_mech_sprites`
    + a `flip_y_at_high_angle` flag on `MechSpritePart`.

### No re-sort by Y mid-tumble (P12-relevant)

- **What we did** — `g_render_parts[]` z-order is fixed: back-side
  limbs first, centerline body, front-side limbs last (with L↔R
  swap when `facing_left`). When a mech dies and tumbles, technically
  the back arm should briefly draw IN FRONT of the body when the
  body rotates past horizontal. We don't re-sort.
- **Why** — Per-mech re-sort by Y costs 17 entries to walk + a sort
  per draw call per dead mech. Spine-based games do it; Soldat
  doesn't. The visible artifact (a back-arm sprite briefly behind
  the chest when it should be in front) lasts a fraction of a second
  during the tumble and then settles correctly once the body stops.
  Tolerable for v1; v2 polish if dead-body presentation becomes a
  visible ship-blocker.
- **Revisit when** —
  - Dead-body rendering reads as visibly broken in playtest (e.g. a
    streamer's footage highlights "the corpse looks weird"). At
    that point: add a per-frame Y-sort over the 17 render parts
    when `m->alive == false`. Cost: a 17-element sort per dead mech
    per draw call. Inside budget at 32 mechs.
  - We add re-sort for any other reason (e.g. carried-flag z-order
    at chest-vs-pelvis tilt). Reuse the same sort path for dead
    bodies.

---

## Rendering kit (P13)

### `FLAG_WINDOW_HIGHDPI` dropped at M6 P03-hotfix; HUD text on 4K monitors with > 100 % DPI scaling now OS-upscaled instead of rendered at native

- **What we did** — `src/platform.c` and `tools/editor/main.c` +
  `tools/editor/shotmode.c` no longer set `FLAG_WINDOW_HIGHDPI`. The
  backbuffer is sized at the window's logical pixel count; on
  HiDPI displays with > 100 % OS DPI scaling, the OS compositor
  upscales the window to the user's physical pixels (well-defined
  bilinear-or-better path on every platform we ship to).
- **Why** — A play-test on a friend's Windows machine with ≈ 192 %
  DPI scaling showed the title-screen UI shifted off the right edge
  of the visible window. With HIGHDPI on, GLFW gave us a framebuffer
  at the monitor's physical pixel count but Windows did NOT scale it
  down to fit the logical window's client area — content centred at
  `GetRenderWidth() / 2` landed at framebuffer ≈ 1920, which on the
  user-visible window (≈ 1999 logical px wide of a 3840-physical
  framebuffer) appeared shoved to the right with the rest of the
  text cropped past the right edge. Post-M6 P03 the internal-RT
  pipeline handles "render at a fixed internal size, blit to window"
  explicitly, so HIGHDPI was buying us nothing and costing real
  players correctness.
- **The cost** — UI text on a true 4K monitor with 200 % DPI is now
  rendered at logical pixel count (e.g. 1920×1080 framebuffer on a
  3840×2160 monitor with 200 % scaling) and OS-compositor-upscaled
  to the display. Reads at the correct size but is bilinear-soft
  compared to native-pixel rendering. The halftone screen's per-
  pixel dither was already designed for a 1080-class internal
  density (see `documents/m6/03-perf-4k-enhancements.md` §1d), so
  this isn't a regression on the *world* — only the HUD glyphs.
- **Revisit if** — anyone reports the HUD reads visibly mushy on a
  HiDPI machine and wants the old behaviour back. The fix is the
  single line in `src/platform.c` (and the matching pair in
  `tools/editor/`). A future polish pass could conditionally re-
  enable HIGHDPI on macOS only (where the compositor DOES scale the
  framebuffer correctly) and leave Windows on the OS-upscale path.

### Per-map kit textures: Foundry only ships parallax (P16-partial)

- **What we did** — P13 shipped the runtime: `src/map_kit.{c,h}` loads
  `assets/maps/<short>/parallax_far.png` / `parallax_mid.png` /
  `parallax_near.png` / `tiles.png` on every `map_build`. **P16**:
  Foundry now ships `parallax_far.png` + `parallax_mid.png` +
  `parallax_near.png` (3 of 4 — `tiles.png` still missing; the M5
  spec-color polygon path covers it). The 7 other maps (Slipstream,
  Concourse, Reactor, Catwalk, Aurora, Crossfire, Citadel) keep the
  M4 flat-color fallbacks. New pipeline step in P16 — Perplexity-
  generated parallax PNGs come back with an opaque dithered canvas;
  `tools/comfy/keyout_halftone_bg.py` keys out the checker pattern
  by local-dark-fraction so foreground silhouettes stay opaque and
  background dither becomes alpha=0. Without this step `parallax_near`
  (drawn AFTER world inside `renderer_draw_frame`) covers the
  chassis underneath. Originals stay in `tools/comfy/raw_atlases/parallax_originals/`.
- **Why** — Asset generation runs on an external service (Perplexity)
  one prompt at a time, and each parallax kit needs three prompts
  (far / mid / near) × eight maps = 24 individual generations.
  Foundry came first as the focus-map for verification (it's also the
  default map in single-player + the most-iterated map). The other 7
  maps' parallax prompts ship in `documents/m5/prompts/` (one per
  map, post-decomposed from the original "all maps" Prompt 5) and
  the team works through them as bandwidth allows.
- **Revisit when** —
  - The remaining 7 maps ship their parallax kits. At that point the
    fallback paths become dead code in real play; the entry deletes.
    Keep the no-asset fallback in source — it's a tiny code surface
    that helps dev-machine builds without asset packs.
  - A custom map ships parallax PNGs that are taller / shorter than
    the screen and the "anchor at top, tile horizontally only" v1
    behavior reads wrong. At that point: add per-map vertical anchor
    metadata to `LvlMeta` (or scale-to-fit-screen-height heuristic
    in `draw_parallax_layer`).

### Decoration sub-rect lookup is hash-based until a manifest ships (P13)

- **What we did** — `LvlDeco.sprite_str_idx` records a STRT byte
  offset like `"decals/pipe_horizontal.png"`. The runtime needs a
  sub-rect inside the shared `assets/sprites/decorations.png` atlas
  to draw it. There's no per-asset manifest at v1 — the renderer
  hashes `sprite_str_idx` into a 16x16 grid of 64x64 cells across
  the 1024×1024 atlas. Stable across runs (same offset → same
  sub-rect) but completely ignores what the path actually says.
- **Why** — Per-deco manifest authoring (a paired `.atlas` file the
  ComfyUI pipeline emits) is a P15/P16 concern. Until the atlas
  exists we want SOME visible feedback for `LvlDeco` records the
  editor places — the hash gives stable placeholder sub-rects that
  at least let designers verify "this deco is on this tile" during
  test-play. When the atlas is missing entirely the renderer paints
  a layer-tinted placeholder rectangle instead.
- **Revisit when** —
  - P15/P16 ships `assets/sprites/decorations.png` AND the paired
    manifest. At that point the hash lookup gets replaced with a
    real `(sprite_str_idx → Rectangle)` map (probably an `stb_ds`
    string-keyed hash table loaded at startup), and the placeholder
    fallback becomes dev-only.
  - The hashed sub-rects collide visibly across maps (different
    decos picking the same cell). 256 cells should be enough for
    M5's deco budget but if a map ships >100 unique decos the
    collision rate climbs.

### Decoration atlas falls back to placeholder rectangles (P13)

- **What we did** — When `assets/sprites/decorations.png` isn't on
  disk, `draw_decorations` paints a 16x16-px (× scale) layer-tinted
  rectangle at each deco position so designers see deco placements
  in test-play. No "hide entirely" path.
- **Why** — Designers using the editor's deco tool (P04) can
  position decos before the atlas exists. Without the placeholder,
  they'd have to mentally translate `LvlDeco` records to expected
  positions — annoying. The placeholder is ~30 LOC including the
  tint table.
- **Revisit when** —
  - Atlas ships at P15/P16. Placeholder path becomes dev-only or
    gets dropped entirely.
  - Designers report the placeholder rectangles are visually
    confusing in shot tests (they look like UI bugs). Add a
    `--no-deco-placeholder` flag or gate behind `--dev`.

### Halftone density is hard-coded at 0.30 (P13)

- **What we did** — `assets/shaders/halftone_post.fs.glsl` reads a
  `halftone_density` uniform in [0, 1]; render.c sets it to
  `HALFTONE_DENSITY = 0.30f` per the spec. There's no config knob.
- **Why** — Per `documents/m5/11-art-direction.md` §"The halftone
  post-process shader" the ship value is 0.30. Surfacing it as a
  config slider is M6 polish — the runtime knob is plumbed through
  `SetShaderValue` so the slider has a place to land.
- **Revisit when** —
  - Playtest reveals the density should differ per-map (e.g. the
    "smelter floor" Foundry feels right at 0.30 but Aurora's open
    sky reads better at 0.15). At that point: add `halftone_density`
    to `LvlMeta` (single Q0.16 field) and read per-map.
  - Accessibility complaint that the screen pattern triggers
    motion sensitivity. Add a "Halftone: off / low / standard"
    toggle to the in-game settings.

### HUD atlas not on disk; bars + crosshair use primitive fallback (P13)

- **What we did** — `assets/ui/hud.png` is referenced by the lazy-
  loader in `hud.c` but doesn't ship at P13. `draw_bar_v2` paints
  the M5 spec layout (1px outline + dark bg + fg fill + tick marks)
  via primitives in BOTH the atlas and no-atlas paths since the
  layout is identical. Crosshair has an atlas-aware sprite path
  but falls back to the M4 line-cross with the new tint colors
  when the atlas is missing. Weapon icons + flag pictograms fall
  back to per-id color swatches + short text labels.
- **Why** — Same staging as the chassis / weapon / parallax / tile
  atlases: ship the runtime first, fill the assets at P15/P16 with
  the ComfyUI HUD-icon pipeline. The fallback shapes (color swatch
  per weapon, "HS"/"GIB"/"OK"/"RAG"/"SUI" for kill flags) keep the
  HUD parseable during P13–P14 development.
- **Revisit when** —
  - P15/P16 ships `assets/ui/hud.png` with proper sub-rects per the
    spec. The fallback paths become dev-only.
  - The color-swatch-per-weapon-id read becomes confusing once we
    have >14 weapons (planned M6 expansion). At that point: drop
    the fallback and require the atlas.

### Decal-layer chunks lazy-allocate; soft cap (P13)

- **What we did** — Levels >4096 px in either dim partition the
  splat layer into 1024×1024 chunks, lazy-allocated on first paint
  per chunk. A typical match stains 3–8 chunks (the high-traffic
  zones); peak memory stays around 12–32 MB. Worst case if every
  chunk gets paint: 28 chunks × 4 MB = 112 MB on a Citadel-sized
  map, past the 80 MB texture budget.
- **Why** — Static-allocating every chunk would push budget past 80
  MB on big maps even if only one chunk ever gets paint. Lazy
  allocation buys the memory headroom for normal play; the worst
  case requires every zone to receive blood, which doesn't happen
  in a 5-minute round.
- **Revisit when** —
  - Playtest on a Citadel-equivalent map shows a long-duration
    match (10+ min) consistently lighting up >20 chunks and
    actually ships over 80 MB. At that point: add an LRU eviction
    pass — chunks not painted in the last 60 s get
    UnloadRenderTexture'd; their decals re-paint from a per-chunk
    "splat archive" the next time the chunk re-allocates. ~80 LOC.
  - We support map sizes >8192×8192 px (M6 stretch). At that point
    the `DECAL_MAX_CHUNKS=64` cap bites and we have to either bump
    or implement true streaming.

### Decal-layer dirty-tracking reserved but not used (P13)

- **What we did** — `DecalLayer.dirty[DECAL_MAX_CHUNKS]` is set when
  a flush paints into a chunk, but `decal_draw_layer` walks every
  allocated chunk every frame and emits one `DrawTextureRec` per
  chunk regardless of dirtiness.
- **Why** — Selective redraw of dirty chunks would save ~10
  textured-quad draws per frame on a Citadel-sized match. Inside
  budget; no perceived hitch. The dirty bit is plumbed for the M6
  selective-redraw path.
- **Revisit when** —
  - Profiling shows the splat layer's draw-pass costing >0.5 ms.
    Implement: track a render-side cached frame; only re-emit the
    DrawTextureRec for chunks whose dirty bit was set this frame.
  - We add the LRU eviction (above) — same dirty bit drives "this
    chunk hasn't been painted in N seconds, free it".

### Hot-reload registration is a fixed startup set (P13)

- **What we did** — `src/hotreload.{c,h}` mtime-watches a fixed list
  of paths registered at startup: chassis × 5 atlases, weapons
  atlas, decorations atlas, HUD atlas, halftone shader. Per-map
  kit textures (`assets/maps/<short>/parallax_*.png` + `tiles.png`)
  reload via `map_kit_load` when the map changes, not via the
  watcher; level files (`assets/maps/*.lvl`) and audio assets
  (`assets/sfx/*.wav`, `assets/music/*.ogg`) are NOT registered.
- **Why** — Per-file registration is simpler than directory-walk +
  glob. The fixed startup set covers ~80% of "designer iterates on
  art and wants to see the change live" — the chassis + weapons +
  HUD atlases are the most-iterated-on assets at the M5 stage.
  Per-map kit assets + `.lvl` + audio are deferred to M6 polish
  when the editor + ComfyUI loop is the gating workflow.
- **Revisit when** —
  - A designer reports they're stuck recompiling / restarting to
    see a parallax change. Switch to a directory-walk hotreload
    mode (`hotreload_register_dir(path, glob_pattern, cb)`) that
    rescans every poll cycle. ~50 LOC.
  - Audio module ships at P14 — wire `.wav` / `.ogg` reloads into
    its own callback (`UnloadSound` + `LoadSound` for each alias).
  - Level reload mid-round becomes a real workflow (e.g. an editor
    F5 round-trip without restarting the host process). At that
    point: add a `LEVEL_RELOAD` host-only debug message that
    triggers `level_load` + `decal_clear` + clients re-download.

---

## World / level

### Hard-coded tutorial map (still called by shotmode + headless harness)

- **What we did** — `level_build_tutorial()` builds the M1 map in code. P01 shipped the `.lvl` loader; the runtime `map_build` path loads `assets/maps/<short>.lvl` (P17 shipped `foundry.lvl`, `slipstream.lvl`, `reactor.lvl`, `concourse.lvl` via `tools/cook_maps`). Shot mode + the headless harness still call `level_build_tutorial` directly because their fixture predates the loader.
- **Why** — Real-match `map_build` loads the authored `.lvl` files; the in-process test harnesses didn't get migrated as part of P17 (out of scope). Migration is a focused refactor: replace `level_build_tutorial(...)` calls in `tests/headless_sim.c` + `src/shotmode.c` with `level_load(...)` against a checked-in test fixture, then delete `level_build_tutorial` from `src/level.c`.
- **Revisit when** —
  - P18 ships the remaining 4 maps + bake-test harness. The harness has its own setup pattern that may pin the tutorial path; tackle migration alongside that work.
  - A regression slips because the shot fixture diverges from `cook_maps`-authored Foundry (e.g., shot tests assume the old M4 cover-wall layout, real play has the P17 hill geometry).
  - The hardcoded slope test bed inside `level_build_tutorial` (separate entry below) is being deleted anyway — fold the retirement of `level_build_tutorial` into that work.

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

### Slope test bed is hardcoded in `level_build_tutorial`

- **What we did** — `level.c::level_build_tutorial` allocates 3 SOLID
  polys (45° / 60° / 5° slopes at floor-row mid-map) for shot mode +
  the headless harness to land on. The runtime `map_build` path used
  by real matches doesn't carry these. P17 shipped four authored `.lvl`
  maps with their own slope vocabulary (30° / 45° / 60° per the per-map
  briefs), but the shot/headless callsites still go through
  `level_build_tutorial`.
- **Why** — Migrating shot tests + the headless harness from
  `level_build_tutorial` to authored `.lvl` files is the same focused
  refactor noted in the "Hard-coded tutorial map" entry above. The
  test bed lives in `level_build_tutorial`, so the two entries delete
  together.
- **Revisit when** — Shot tests + headless harness load `.lvl` fixtures
  directly via `level_load`. At that point both this entry and the
  "Hard-coded tutorial map" entry delete simultaneously.

### M5 maps are synthesized programmatically, not editor-authored (P17 + P18)

- **What we did** — `tools/cook_maps/cook_maps.c` emits ALL eight M5
  `.lvl` files via the same programmatic builder pattern (the P01
  one-shot stub became this exporter):
    - **P17** (2026-05-11): `foundry` / `slipstream` / `reactor` /
      `concourse`.
    - **P18** (2026-05-12): `catwalk` / `aurora` / `crossfire` /
      `citadel`.
  The per-map briefs in `documents/m5/07-maps.md` expect every layout
  to be hand-authored in the P04 editor (~2-4 hours of design work per
  map); the cook_maps scaffolds match each brief's intent (slope
  vocabulary, alcove counts, pickup density, spawn lanes, ambient
  zones, CTF flag positions) but skip the iterative layout-pass
  discipline the editor enables. Concourse's wing-floor "valleys" are
  POLY_KIND_BACKGROUND (purely visual), not physics, because dig-in
  geometry below the floor row needs editor-driven polygon authoring.
  Citadel's tunnel grades are short triangles approximating the spec's
  "gentle 30° rise/fall" rather than full mesh sculpting. Aurora's
  "30+ skyline silhouettes" ship as `POLY_KIND_BACKGROUND` triangles
  rather than parallax-layer art (the deco/parallax atlas hasn't shipped
  for the new maps — separate trade-off).
- **Why** — P17/P18 each ran in a single session without interactive
  access to the editor. Shipping programmatic scaffolds gets all 8
  maps into the rotation immediately and lets the bake-test pass run
  against them; a designer refines layouts in the editor afterward,
  saving over the named `.lvl` files. The cook_maps source is the
  canonical record of each starting layout if an editor-authored
  version goes wrong.
- **Revisit when** —
  - A designer iterates any map in the editor; the saved `.lvl`
    overwrites cook_maps' output. At that point **rerunning
    `make cook-maps` would clobber editor work** — either drop the
    relevant builder from cook_maps once a designer takes ownership,
    or have cook_maps refuse to overwrite a newer `.lvl` (mtime
    check, ~10 LOC).
  - The bake-test reveals map-specific dead zones the programmatic
    layout couldn't anticipate (an alcove never visited, a Rail
    Cannon spot uncontested, an asymmetric crossfire spawn imbalance,
    Citadel tunnels under-trafficked, etc.). Editor iteration is the
    natural fix.
  - Wing-floor valleys' visual-only status reads weird at gameplay
    distance (the player expects to drop into a basin). Either replace
    them with SOLID polys (might require a deeper floor row) or remove
    them entirely from the brief.

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

### Carrier secondary fully disabled, not partially (P07)

- **What we did** — `mech_try_fire` short-circuits when
  `m->active_slot == 1 && ctf_is_carrier(w, mid)`. The flag carrier can
  fire their primary normally; their secondary slot is wholly inert
  while carrying. P09's `BTN_FIRE_SECONDARY` (RMB one-shot) inherits
  the same gate when it lands.
- **Why** — Soldat's CTF rules allow some secondaries (knife / nades
  with reduced ammo) while disabling others. Implementing per-weapon
  rules adds a `Weapon.carrier_allowed` flag plus per-secondary tuning
  and isn't load-bearing for getting the round-loop right. The fully-
  disabled simplification reads as "you can defend with primary,
  not stack utility on top" — a coherent rule players can predict.
- **Revisit when** —
  - Playtest reveals the half-jet + no-secondary penalty over-stacks
    and capturing feels impossible. Loosen the secondary rule first
    (knife / micro-rockets re-enabled) before touching the jet rule.
  - We add a non-damage utility secondary (e.g. smoke grenade) that
    the carrier obviously SHOULD be able to use defensively.

### Auto-return is 30 s flat (no fading visual countdown) (P07)

- **What we did** — Dropped flags auto-return after exactly
  `FLAG_AUTO_RETURN_TICKS = 30 * 60` ticks (30 s @ 60 Hz). The wire
  format ships `return_in_ticks` so a HUD timer is possible, but the
  current HUD doesn't render a countdown — the only visual cue is a
  faint outline halo on `FLAG_DROPPED` flags via `render.c::draw_flag`.
- **Why** — Threewave / UT have a "flag pulses faster as return is
  imminent" pattern that requires a per-frame shader tween + a HUD
  timer. v1 ships the simple constant timer; visual urgency is the
  player knowing the rule (30 s) and counting in their head if they
  care. Most rounds the flag is grabbed long before auto-return fires
  anyway.
- **Revisit when** —
  - Playtest reveals players regularly losing dropped-flag awareness
    in the last 5 s (a common Threewave complaint). Add either a
    pulse shader to draw_flag or a small "auto-return in N s"
    timer to the HUD pip.
  - The HUD final-art pass (P13) lands and we want the dropped flag
    to read at a glance — natural carrier for the pulse work.

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

### Map-shared assets resolve to client-local files (P08)

- **What we did** — A custom `.lvl` streamed from a server (P08) is
  validated, cached at `<XDG_DATA_HOME>/soldut/maps/<crc>.lvl`, and
  loaded on round start. But the level's string-table-indexed asset
  references (background PNG, music OGG, ambient loop, etc.) are still
  resolved against **the client's local files**. A custom map that
  references `"music/citadel.ogg"` plays whatever the client has at
  `assets/music/citadel.ogg`, not what the server has on disk.
- **Why** — Bundling the audio + parallax assets alongside the `.lvl`
  would multiply the on-wire payload from a ~500 KB level file to
  several MB per map, plus require either (a) a multi-file "map bundle"
  format (.tar / .zip on the wire) or (b) a manifest + content-addressed
  per-asset download. Both are real work and don't matter until P14
  (audio module) and P13 (parallax) actually consume those references.
  At P08 we ship the `.lvl` only; missing-asset references fall back to
  the existing per-map defaults baked into the runtime.
- **Revisit when** —
  - P14 ships audio and a custom map's missing music is noticeable in
    playtest.
  - P13 ships parallax and a custom map's missing background reads as
    visibly broken (skybox missing, etc.).
  - A community starts shipping custom maps with custom assets and the
    workflow becomes visibly missing.
  - At that point: design a "map bundle" format (probably a tarball with
    the `.lvl` + a manifest of asset paths + content-addressed asset
    files) and extend the wire protocol to ship the bundle in chunks
    instead of just the `.lvl`. The cache directory shape stays the
    same; just `<crc>.bundle` instead of `<crc>.lvl`.

### No server-side download throttling (P08)

- **What we did** — `server_handle_map_request` on receiving a
  `NET_MSG_MAP_REQUEST` immediately reads the entire `.lvl` from disk
  and `enet_send_to`s every chunk back-to-back on the reliable channel.
  No per-peer throttling, no bytes-per-second cap, no fair-queueing
  across simultaneous joiners. ENet's reliable channel handles
  retransmission backpressure but we don't shape the queue.
- **Why** — At P08's scale (LAN play; small maps ≤50 KB; rare joins)
  ENet's own buffering is fine. A 500 KB map at the spec's worst case
  is ~430 chunks — drained from the host's outgoing queue in one second
  on LAN. A burst of 8 simultaneous joins requesting the same 500 KB
  map = ~4 MB host upstream within a second; ENet absorbs that without
  visible stutter on a typical home connection. The proper fix
  (per-peer in-flight-bytes cap + a queue that round-robins across
  peers) is a half-day of work that solves a problem we don't have yet.
- **Revisit when** —
  - A host runs a public server and reports stutter / lobby UI
    unresponsiveness when many clients connect simultaneously.
  - Maps grow past 1 MB (per P17/P18 authored content) and the
    upstream burst at simultaneous-join becomes a real bottleneck.
  - We hit `NET_MAP_MAX_FILE_BYTES = 2 MB` as a real ceiling and have
    to consider "what happens when 16 peers all download a 2 MB file
    at once" (32 MB upstream burst — no longer trivial).

### No download resume across host process restarts (P08)

- **What we did** — `NET_MSG_MAP_REQUEST` carries a `resume_offset`
  field for "I had a partial download from before, please continue from
  byte N." The server honors it (fseek's the file to that offset before
  streaming). But the client's `MapDownload` lives in process memory:
  if the client process restarts, the partial buffer is gone and
  resume_offset is always 0 on the next connect. Worse, if the **host**
  restarts mid-download, its `server_map_desc.crc32` may have changed
  (different .lvl contents in `assets/maps/`), and the client's
  resume_offset is meaningless against the new file.
- **Why** — Persisting partial downloads across process restarts wants
  either (a) a per-`<crc>.partial` file in the cache that we extend on
  successive runs, plus a resumed-state index, or (b) treating every
  fresh process as starting over (current behavior). At P08 the
  download is fast enough (~1–3 s LAN, ~5 s WAN at 500 KB) that
  resume-across-restarts is not load-bearing. The wire format reserves
  `resume_offset` so we can add the persistence path later without
  bumping the protocol.
- **Revisit when** —
  - Maps grow past 1 MB AND we ship over WAN at typical home upload
    speeds (~500 KB/s) — at that point a 2 MB map takes ~5 s and a
    crash mid-download is more likely to be a noticeable lost minute.
  - We do an internet-public test with 50 ms+ RTT and observe that
    half-finished downloads after a quick reconnect re-pull from byte 0.
  - At that point: write the partial buffer to `<cache>/<crc>.partial`
    on every chunk apply (or at least every N seconds), index pending
    crcs in a small `partials.txt` so the client knows what to resume
    on next connect. The host already trusts the resume_offset; only
    client-side state needs persisting.

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

### Snapshot pos quant factor 4× → ~8190 px max world width

- **What we did** — `quant_pos` in `src/snapshot.c` and the parallel
  encoders in `src/net.c` (projectile state, hit/fire events, flag
  state) pack world positions as `int16_t` with a 4× sub-pixel factor.
  Range: ±8190 px. Resolution: 0.25 px.
- **Why** — Originally the factor was 8× (range ±4096 px, 0.125 px
  resolution). The Crossfire CTF map is 4480 px wide, so anything
  east of x=4096 (the entire BLUE base, including the BLUE flag at
  x=4160) silently wrapped to x=4095 in the wire format. The
  symptom on the wire was hard to spot — user-visible behavior was
  "the client gets stuck on their own flag" because every snapshot
  jammed the client's local mech back to x=4095, even as the server
  simulated past it. Cutting the factor in half doubled the range and
  kept sub-pixel precision well below renderer-interp jitter.
- **Revisit when** —
  - We ship a map wider than ~8000 px. Either bump down to a 2× factor
    (max ±16380 px, 0.5 px res) or move position to `int32_t`.
  - We add per-frame visible reconcile jitter that wasn't there before.
    0.25 px steps could in principle stair-step the local mech under
    extreme low-velocity conditions; not observed in current play.
  - Bandwidth becomes the bottleneck (current snapshot is well under
    budget — 22 bytes per mech × 32 mechs × 30 Hz = 21 KB/s). At
    that point you'd encode delta+varint and the fixed factor
    becomes irrelevant.

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

### Map-vote-card thumbnails are placeholder gray rectangles (P09)

- **What we did** — The 3-card map vote picker on the summary
  screen renders each map as a fixed gray rectangle at the top of
  the card; only the display name + (truncated) blurb identify the
  map.
- **Why** — Real map preview thumbnails want a render-pass that
  walks the level geometry (or a screenshot baked at editor save).
  That's a couple of hours of art-pipeline work and only matters
  once the maps look distinct enough that thumbnails would help
  picking. P09 is the plumbing pass; thumbnails ride P13/P16's art
  pipeline.
- **Revisit when** —
  - P13 (rendering kit) lands — the same atlas / decal pipeline
    can output 1×-scale snapshots of each level into
    `assets/maps/<short>_thumb.png`.
  - P17/P18 ship 8 authored maps — at that point distinctive
    thumbnails become real navigation aid (the map names alone
    blur together when you have 8 of them in rotation).

### AOE explosion has a ~RTT/2 visual gap on the client (wan-fixes-10)

- **What we did** — Frag grenade / rocket / Mass Driver dud /
  plasma orb all detonate AOE on a `NET_MSG_EXPLOSION` event the
  server broadcasts in `apply_new_kills`-style fashion from
  `broadcast_new_explosions`. The wire payload is 7 bytes (pos at
  1/4 px quant + owner mech + weapon_id; client looks up
  radius/damage/impulse from `weapon_def`). The client's local
  visual grenade (spawned from `FIRE_EVENT`) dies silently in
  `projectile.c::detonate` when authoritative=false — no
  `explosion_spawn` call. The event handler `client_handle_explosion`
  kills any matching alive projectile from the same owner + spawns
  the visual via `explosion_spawn` (which on the client paints
  sparks + sfx + screen shake and early-returns before the damage
  loop).
- **Why** — Per wan-fixes-3 remote mechs are kinematic on the
  client in REST POSE; the server runs animated bones. A bouncy
  frag rolling on the floor detonates against the target's
  R_KNEE at world x=480 on the server (animated knee pos), at
  world x=489 on both clients (rest-pose knee pos) — 9 px off.
  Pre-fix the client's local detonate painted the explosion at
  the wrong location while damage applied at the server's
  location. Routing the visual through the server event makes
  it match where damage actually lands.
- **Cost** — Between the moment the client's visual grenade
  silently dies and the moment `NET_MSG_EXPLOSION` arrives the
  user sees no projectile + no boom — ~RTT/2 gap. On localhost
  it's a single frame and invisible. On WAN (~50–150 ms RTT)
  it's a ~25–75 ms dropout the user might perceive as a brief
  pause. Acceptable vs the alternative (boom in the wrong
  place). Also, non-AOE projectiles (Pulse Rifle / Sidearm /
  hitscan) still detonate visually on the client at impact —
  the trade-off only applies to AOE projectiles where the
  server's auth position diverges from the client's local sim.
- **Revisit when** —
  - WAN ping crosses ~200 ms regularly and the gap reads as a
    glitch. Mitigations: the client could keep its local
    projectile alive for ~RTT after detonate (waiting for the
    event) and only paint visual at the server's coords once
    received; OR ship full bone-state in snapshots so the
    client's projectile_step sees the same bone positions as
    the server (defeats wan-fixes-3 cost).
  - A future cosmetic-vs-authoritative split lets the client
    render a "pending detonation" sprite at the local impact
    until the event lands — bridges the gap visually.

### Kill-feed names truncate at 16 bytes (wan-fixes-13)

- **What we did** — `NET_MSG_KILL_EVENT` payload widened from 7
  to 39 bytes (u16 killer + u16 victim + u16 weapon + u8 flags
  + 16-byte killer_name + 16-byte victim_name). The lobby
  allows up to `LOBBY_UI_NAME_BYTES = 24` for player names; the
  wire truncates to the first 15 characters + NUL terminator.
  `KillFeedEntry.killer_name` / `victim_name` are the same 16-
  byte storage on the World.
- **Why** — Names ride every kill event reliably; a 24-byte
  field would push the kill-event packet to 55 bytes per kill,
  not catastrophic but the visual layout of the kill feed only
  has room for ~12-character names before they collide with the
  weapon icon. Capping at 16 keeps the wire under 40 bytes and
  the HUD layout clean.
- **Cost** — Players with names longer than 15 characters get
  truncated in the kill feed (their full name still shows in
  the lobby + scoreboard, which uses the slot's `name[24]`
  directly). Most names are well under 12 characters; few users
  notice.
- **Revisit when** —
  - A friend group consistently uses long handles (e.g.
    `GamerTagXyz_2026`) and complain about truncation.
  - Mitigation: bump the wire to 24 bytes per name (kill event
    grows to 55 bytes), OR ship a slot_id-keyed lookup so the
    event carries `u8 killer_slot` + `u8 victim_slot` and the
    client resolves names from `g->lobby.slots[].name`
    (untruncated). The latter needs the lobby table to be
    reliably populated on the client at the moment of kill —
    P09's lobby-list broadcast pattern makes that mostly true
    but not guaranteed.

### Host UI's dedicated server runs in a thread, not a child process (wan-fixes-5 → wan-fixes-16)

- **What we did** — "Host Server" in the title screen calls
  `host_start_begin` which spawns a THREAD inside the host UI
  process (not a child process) via
  `in_process_server_start`. The thread runs `dedicated_run`
  against a separate `Game` struct and binds its UDP socket on
  port 23073. The host UI's main thread then connects as a
  regular client to `127.0.0.1:23073`. Standalone `--dedicated`
  CLI still exists for external dedi servers; `--listen-host`
  is retained for offline solo + shotmode tests.
- **Why** — wan-fixes-5 originally spawned a `--dedicated PORT`
  child of the same binary. On Windows this is broken at the OS
  level: any UDP packet between two processes that share a
  spawn-tree ancestor is silently dropped. Confirmed on two
  separate machines with five workarounds (handle inheritance
  off, `CREATE_NO_WINDOW`, `CREATE_BREAKAWAY_FROM_JOB`, a
  launcher-pattern that orphans the dedi from a now-dead
  intermediate, and `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS`
  spoofing to Explorer). The filter sticks regardless of kernel
  PPID, destination IP, or job-object membership — almost
  certainly an AV / WFP callout tracking process ancestry
  independent of the kernel's PPID field. Same-process UDP
  loopback DOES work (proved with a dedi self-loopback probe),
  so the fix is to not have a second process at all.
- **How it preserves the dedicated-server philosophy** — Two
  fully separate `Game` structs (UI client + server), zero
  shared memory between threads, communication strictly over
  the kernel UDP socket pair. Host's input goes through
  `net_send_input` → kernel UDP → server's `net_poll` exactly
  like any other peer. Server tick / snapshot broadcast runs on
  its own cadence on its thread. Host has no zero-latency
  advantage.
- **Cost** —
  - Threading: this is the first thread in the v1 codebase
    (CLAUDE.md previously said "no threads at v1"). The thread
    is fully isolated — no shared mutable state with the main
    thread — so the synchronization burden is minimal (one
    `volatile sig_atomic_t s_in_proc_server_ready` and one
    `volatile sig_atomic_t s_in_proc_server_quit`).
  - Memory: ~60 MB extra for the second `Game` struct (arenas
    + pools). Acceptable on modern hardware.
  - Server crash takes down the UI (no process isolation).
    Acceptable v1 — the server doesn't allocate or run any
    obviously fault-prone code.
  - raylib's GL context lives on the main thread; the server
    thread is forbidden from calling raylib. `decal_init` is
    gated standalone-only because `LoadRenderTexture` needs the
    GL context. Other raylib touch-points are already gated by
    `IsWindowReady()` checks that no-op when called from a
    non-GL-context thread, but new server-side code MUST stay
    raylib-free.
- **Revisit when** —
  - The Windows UDP filter behavior changes (e.g., a future
    Windows version or AV update no longer drops same-spawn-tree
    UDP). Re-introducing the spawn-child path would restore
    process isolation but is currently impossible on the
    machines we've tested.
  - We need crash isolation (e.g., the server runs heavy
    third-party code that might fault). At that point a
    spawn-child path could be conditionally reintroduced on
    non-Windows platforms.

### Host-setup overlay can't cancel mid-spawn (wan-fixes-9)

- **What we did** — While the polled bootstrap is in flight
  (`L->host_starting == true`), `host_setup_screen_run` disables
  both Back and Start Hosting via the existing `ui_button`
  disabled branch. The user has no in-UI way to abort.
- **Why** — Aborting mid-spawn would orphan the
  `s_host_start.child` handle. Cleanly cancelling needs a
  cancel-request flag on the static spawn state + a teardown
  path in `host_start_abort`; doable but small scope. The
  current escape hatch is window-close (which calls
  `host_start_abort` from the shutdown path).
- **Cost** — User who clicked Start Hosting by mistake has to
  wait ≤5 s for the timeout or close the window. Annoying but
  rare; cost is friction, not lost work.
- **Revisit when** —
  - Players report being stuck staring at the spinner. Wire a
    visible Cancel button into the overlay that calls
    `host_start_abort` + returns to MODE_TITLE.

### Prefs file is per-cwd, not per-user (wan-fixes-8)

- **What we did** — `prefs_load` / `prefs_save` use
  `PREFS_PATH = "soldut-prefs.cfg"` — relative to the process's
  current working directory. Mirrors the `soldut.cfg` and
  `bans.txt` convention so the file lives next to the binary in
  artifact zips.
- **Why** — A platform-correct per-user path (XDG on Linux,
  `~/Library/Preferences/Soldut/` on macOS, `%APPDATA%\Soldut\`
  on Windows) would need the `getenv` + `mkdir -p` plumbing
  that's already in `src/map_cache.c::resolve_cache_dir`. Extra
  scope; the cwd-relative path works fine for users who
  download the artifact zip + run from there. Users who run
  multiple Soldut binaries in different directories get
  per-install prefs — debatable whether that's a bug or a
  feature (a "competition build" gets its own loadout pick).
- **Cost** — Players running from multiple shells with
  different cwds get different prefs files. Players running on
  Windows from a write-protected Program Files install get
  prefs save failures (no error surfaced beyond LOG_W).
- **Revisit when** —
  - User reports "my prefs reset whenever I move the .exe."
  - Mitigation: copy `resolve_cache_dir`'s pattern from
    `map_cache.c` into `prefs.c`; `PREFS_PATH` becomes the
    fallback when the platform path can't be resolved.

### Bans are by display name only (no IP) (P09)

- **What we did** — `net_server_kick_or_ban_slot` calls
  `lobby_ban_addr(L, /*addr*/0, ts->name)`. The `bans.txt` file
  stores `addr_hex name` per line; with addr=0 only the name match
  fires in `lobby_is_banned` (the address branch is gated on
  `addr != 0`).
- **Why** — IP bans want the peer's resolved `remote_addr_host`
  threaded through the kick path (the field exists on `NetPeer`
  and `lobby_is_banned` already supports addr matching) — but on a
  shared LAN multiple players can NAT through one address, so a
  hard-IP ban can punish unintended bystanders. Display-name bans
  are good enough for the trolling-friend use case the host UI
  serves; the wire format already carries the `addr_hex` field
  ready to populate when we want it.
- **Revisit when** —
  - A host runs a public WAN server and reports "banned player
    keeps coming back with a new name."
  - We add display-name validation that prevents trivial rename
    bypass (e.g. `Bob1` → `Bob2`).
  - At that point: change the kick path to grab
    `g->net.peers[i].remote_addr_host` before
    `enet_peer_disconnect_later` and pass it as the `addr` arg to
    `lobby_ban_addr`. The `bans.txt` schema already supports both
    fields; only the `addr=0` literal in the kick handler needs to
    move.

---

## Audio (P14)

### No crossfade between map tracks (P14)

- **What we did** — `audio_set_music_for_map(path)` does a hard
  stop + unload + reload + play sequence on the same `Music` slot.
  Round transitions cut the previous track and start the new one
  with a brief silence in between (during the OGG header parse +
  buffer fill).
- **Why** — Crossfading wants two simultaneous decoders + a mix
  envelope on each — doubles the streaming buffer (1 MB each, vs the
  one we have) and adds non-trivial state (per-track gain ramp,
  envelope arithmetic). The hard cut reads "round-end" cleanly to
  players ("the music stopped, the next one started"). Spec
  pre-disclosed this as a deliberate compromise.
- **Revisit when** —
  - Playtest reports the round-transition silence reads as broken
    (e.g., "the music drops out, did the game freeze?"). Add a 200 ms
    fadeout on the outgoing track + 200 ms fadein on the incoming
    via the existing `audio_set_bus_volume` infrastructure (or by
    modulating MUSIC bus duck_target through the transition).
  - We add a "lobby music" track that should crossfade into match
    music — at that point the dual-decoder cost might be worth it.
    M6 polish.

### No 3D HRTF / binaural pan (P14)

- **What we did** — `audio_play_at` computes a single linear pan
  from `dx / 800` (clamped to [-1, +1]) and a linear distance
  attenuation from `1 - clamp((dist - 200)/1300, 0, 1)`. raylib's
  `SetSoundPan` distributes the resulting signal between L/R speakers.
  No HRTF filter, no head-shadow simulation, no Doppler shift.
- **Why** — We're a 2D side-scrolling game on a fixed-camera plane.
  HRTF would simulate height + distance + occlusion that doesn't
  exist in the play model. Linear pan + distance attenuation is what
  the spec calls for and matches every comparable 2D shooter.
- **Revisit when** —
  - VR / 3D spatial-audio modes ever come into scope. Out of v1.
  - Players report "I can't tell where the sound came from"
    consistently. Distance attenuation + pan on stereo speakers is
    well-understood; if it fails, the cure is louder cues, not HRTF.

### Servo loop only for the local mech (P14)

- **What we did** — `audio_step` reads the local mech's pelvis
  velocity each frame and calls `audio_servo_update(vel)`; remote
  mechs get no servo loop. Local-only servo means a quiet stationary
  player + audible running, but two players running side-by-side on
  the client only hear one servo (the local mech's).
- **Why** — Three reasons per `documents/m5/09-audio.md`:
  (a) 32 simultaneously-modulated servo loops would be a CPU + audio
  graph nightmare for a per-mech-feel cue that doesn't carry tactical
  information; (b) a single intimate loop attached to the listener
  reads more authentically than a chorus of distant servo hums;
  (c) spatializing 32 loops via `audio_play_at` doesn't fit the
  one-source-per-Sound model — we'd need 32 dedicated `Sound`
  instances. Pre-disclosed by the spec.
- **Revisit when** —
  - We add a stealth mechanic where hearing nearby mechs' servos is
    tactical signal (e.g., "I can hear them coming"). At that point
    the cleanest path is a per-Mech `Sound` instance with distance
    attenuation, capped at the nearest 4 mechs.
  - We ship variable-pitch chassis-specific servo loops (Heavy
    deeper, Scout higher) — at that point per-listener servo audio
    might want per-chassis variants for own-mech too.

### Footstep SFX is concrete-surface only at v1 (P14)

- **What we did** — `build_pose`'s ANIM_RUN gait wrap fires
  `audio_play_at(SFX_FOOTSTEP_CONCRETE, ...)` for every plant. Metal
  + ice surfaces have manifest entries (and the sound files will
  drop in at P15+) but no surface-detection logic queries the foot's
  contact tile to pick the right SfxId.
- **Why** — Surface detection wants a `level_tile_at(foot_pos)`
  query + an SfxId map keyed off `LvlTile.kind` and (later) tile-id
  ranges in the atlas. Cheap to write but adds a layer of conditional
  logic that's only meaningful once the actual sound files exist
  (otherwise all three play silence). Defer to playtest after P15+
  ships at least one footstep variant.
- **Revisit when** —
  - P15-P18 ship `assets/sfx/footstep_concrete.wav` AND any other
    surface variant. At that point: in `build_pose`, query
    `level_tile_at` at the plant position, branch on `LvlTile.kind`
    (TILE_ICE → SFX_FOOTSTEP_ICE; TILE_SOLID with metal-id range →
    SFX_FOOTSTEP_METAL; else → SFX_FOOTSTEP_CONCRETE). ~15 LOC.
  - We add per-chassis footstep variants (Heavy thuds, Scout pads).
    Same dispatch site; just more SfxIds.

### Held-jet pulse rate-limit picked by integer tick count (P14)

- **What we did** — `apply_jet_force` fires `SFX_JET_PULSE` once
  every 4 ticks (`if (w->tick - last_jet_pulse_tick >= 4)`) using a
  per-Mech `last_jet_pulse_tick` field. At 60 Hz that's ~15 pulses/sec
  per held jet. The cue plays on whichever tick the inequality first
  fires after a press.
- **Why** — The jet "puff" sound design is a continuous-textured
  cue (per-tick is way too dense; sub-3-tick reads as a stuttering
  burst); 4 ticks = 67 ms is roughly the resolution of human pulse
  perception for layered SFX. Picking the rate as a fixed tick
  delta — instead of a `Sound` loop — keeps the fire site simple
  + lets the rate change per-chassis later (Burst-jet wants faster).
- **Revisit when** —
  - We add per-chassis jetpacks with distinct exhaust character
    (Burst chassis already has a separate SFX_JET_BOOST for the
    BTN_DASH dump). Per-chassis pulse rate just becomes a chassis
    field; lookup at the rate-limit check.
  - Audio designers want a continuous looping jet pulse (more
    "afterburner" than "puff puff puff"). At that point: replace
    the per-tick fire with a held `Sound` similar to the servo loop,
    modulated by the held-jet state.

### Client-side mech_kill audio is server-only (P14)

- **What we did** — `mech_kill` fires `audio_play_global(SFX_KILL_FANFARE)`
  for the local killer and `audio_play_at(SFX_DEATH_GRUNT, victim_pos)`
  for the victim's pelvis. `mech_kill` runs only on the authoritative
  side (server / offline-solo); pure clients see death via snapshots
  flipping `alive = false`, never call `mech_kill` locally, so they
  miss both audio cues for their own deaths and any kills they
  scored. Hits arrive via `client_handle_hit_event` which already
  plays `SFX_HIT_FLESH`, but the killing-blow fanfare/grunt is silent.
- **Why** — The server-authoritative side is the right place for
  the kill bookkeeping (kill feed, score, dismember mask, drop-flag
  on death). Replicating death audio onto the client wants either
  (a) a `NET_MSG_KILL_EVENT` that the client handles, or (b)
  detecting the snapshot transition `alive: true → false` per mech
  and firing audio. (b) is cheaper but spec'd separately from the
  HIT_EVENT path — defer to a focused fix when the gap shows up
  in real WAN play.
- **Revisit when** —
  - WAN playtest: clients consistently report "I can't tell when I
    got the kill" — at that point either gate the existing fire
    audio's `predict_drew` machinery into a "this fire connected"
    bit, or add the snapshot-edge detector in `snapshot_apply`.
  - We add a kill-streak HUD or announcer cue — needs the same
    death-detection event regardless of network role.

### UI hover sound not wired (P14)

- **What we did** — `ui_button` plays `SFX_UI_CLICK` on press; no
  hover-enter sound. The manifest declares `SFX_UI_HOVER` (and the
  asset path is reserved), but the runtime doesn't fire it.
- **Why** — Hover detection wants per-button state tracking ("was
  this rect hovered last frame too?"). The current `UIContext` is
  intentionally stateless across frames apart from focus + caret.
  Adding `last_hovered_id` (hash of rect + label) on UIContext +
  comparing per call is ~10 LOC, but the audio benefit is small
  relative to click — players notice clicks more than hovers, and
  a busy UI plays hovers constantly which becomes its own noise
  problem.
- **Revisit when** —
  - The HUD final art pass calls for hover audio as part of menu
    polish (likely M6). At that point: add a `last_hover_hash`
    field on UIContext, hash `(rect.x, rect.y, label[0])` per
    button to identify it, fire SFX_UI_HOVER on hash transition.
  - We add a settings menu where hover navigation is the primary
    interaction model. Same fix.

---

## Jet propulsion FX (M6 P02)

### Scorch decals are permanent for the round (no per-decal fade)

- **What we did** — `decal_paint_scorch(Vec2, float)` mirrors the
  `decal_paint_blood` shape — queue → flush into the splat-layer
  RT(s) inside one `BeginTextureMode` pair per dirty chunk. Once
  painted, the pixels are baked into the RT and stay until the
  level is rebuilt (round transition). The spec's "~2 s fade
  window" never lands.
- **Why** — The decal layer has no per-splat age tracking; it's
  literally just painted pixels into a render texture. Adding a
  parallel ring buffer of splat records (pos, age, fadable_color,
  source_chunk) so the flush path can decrement age + re-paint
  faded pixels every frame is real work — a separate data
  structure, a per-frame walk, and a tricky "what does fade look
  like on an alpha-blended pixel that was painted over by another
  splat 50 ms later" interaction. We elected to ship without it;
  the binary-on/permanent behavior reads fine against the rest of
  the kit (blood decals already use the same permanent model).
- **Revisit when** —
  - The decal layer grows per-splat age records for any reason
    (M6 polish on blood, the planned "blood dries to brown" feature,
    etc.). Once the structure exists, scorch slots straight in.
  - Playtest reports the scorch marks accumulating into visual
    clutter on long matches. The chunk-rotation policy already
    overwrites oldest pixels in a high-traffic chunk; if that's
    not enough, an explicit fade is the next move.

### Jet exhaust is client-local (no per-particle wire data)

- **What we did** — Every client spawns its own jet-exhaust + dust
  + scorch FX from the (replicated) `Mech.jet_state_bits` +
  `chassis_id` + `jetpack_id` + `facing_left` + `grounded` state.
  Particles are spawn-distribution-identical across clients (same
  spawn rates, same color, same source position) but NOT particle-
  bit-identical (each client's `world.rng` advances independently
  through other FX paths). Two clients watching the same jetting
  mech see "the same plume" — same length, color, density — but
  the individual exhaust dots scatter differently.
- **Why** — Per-particle networking would require ~5+ bytes per
  spawn × dozens of spawns per active jet per tick × every jetting
  mech × every snapshot — order-of-magnitude bandwidth blowout for
  a purely visual layer. The existing `NET_MSG_HIT_EVENT` /
  `NET_MSG_FIRE_EVENT` pattern (replicate the EVENT, let each
  client spawn its own particles from it) is the established
  precedent; jet FX follows the same model. The FX driver is a
  pure read of replicated state — host and client run the exact
  same code path and produce statistically-identical streams.
- **Revisit when** —
  - Playtest reports specific particle landing positions diverging
    enough between clients to be a reportable bug ("my exhaust hit
    that wall, theirs didn't"). The FX particles don't interact
    with gameplay so this would be a pure aesthetic complaint.
  - We add particle-driven gameplay (e.g. exhaust pushes other
    players' aim, dust obscures sightlines for damage purposes).
    At that point the FX layer becomes simulation, and per-particle
    wire data is the right answer.

### Heat shimmer uniform loop has a 16-zone hard cap (one per active mech, at pelvis)

- **What we did** — `halftone_post.fs.glsl` carries a fixed
  `vec4 jet_hot_zones[16]` uniform; `mech_jet_fx_collect_hot_zones`
  writes ≤16 entries by walking alive mechs with `MECH_JET_ACTIVE`
  set. To fit the cap, we collect ONE zone per active mech (at the
  pelvis) instead of one per nozzle — so a 2-nozzle Burst-jet
  contributes a single shimmer disc covering both plumes rather
  than a per-nozzle pair.
- **Why** — `MAX_LOBBY_SLOTS = 16` matches the cap exactly; any
  more zones than active mechs is wasted shader work. A larger
  uniform array (e.g. 32 for per-nozzle) is feasible but doubles
  the per-fragment loop cost at 1920×1080 (~2M pixel-iters × N
  zones). The per-mech-at-pelvis radius (40 px sustain, 80 px
  boost, 120 px ignition) is wide enough to cover both nozzles
  for any 2-nozzle jetpack on the current chassis size band.
- **Revisit when** —
  - `MAX_LOBBY_SLOTS` grows beyond 16. The cap shifts; the cleanest
    response is to keep one-zone-per-mech but bump the array size
    to match.
  - Per-nozzle visual differentiation becomes important (Burst's
    twin plumes shimmer independently as a stylistic statement).
    At that point: 32-slot uniform, per-nozzle collection, and
    a perf check on integrated graphics at 1080p.

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

### Bake-test verdict is informational, not gating (P18)

- **What we did** — `tools/bake/run_bake.c` runs `simulate_step` with
  N crude wandering bots for `duration_s` seconds (default: 60 s ×
  16 bots) and writes per-map heatmap PNG + CSVs + summary. The
  verdict passes if the sim ran without crashing AND bots produced
  motion + fire events. The per-map acceptance criteria in
  `documents/m5/07-maps.md` ("all 12 pickups grabbed at least once",
  "Mass Driver grabbed 8+ times over 10-min bake", "no dead zones",
  "captures within 30% red/blue") are designer-facing targets, not
  automated pass/fail rules.
- **Why** — The bot AI is intentionally crude per the brief: wander
  toward a random spawn point; aim at the nearest enemy in 800 px
  line-of-sight; shoot. No flag-running heuristic, no pickup priority,
  no path planning. 60 s × 16 bots produces sparse pickup-grab data
  on big maps (Citadel typically grabs 6-7 of its 31 pickups;
  Crossfire grabs 3-4 of 32). Tighter acceptance would require 10-min
  runs (per the brief) plus smarter bots — neither is in scope for
  P18. The heatmap PNG is the actionable artifact: designers eyeball
  it for dead zones and spawn imbalance.
- **Revisit when** —
  - A designer wants the bake to be CI-gating. At that point: smarter
    bots (per-map A* nav, pickup priority, flag-running for CTF),
    longer default duration, and per-map acceptance rules in
    `run_bake.c::compute_verdict`.
  - Real playtest data invalidates the heatmap-based judgement
    (players don't go where crude bots go). At that point use
    playtest CSV/replays as the bake-test input, not bots.
  - Bot AI improvements arrive (M6 stretch) — promote the verdict
    from informational to gating with per-map thresholds.

### No CI for physics correctness

- **What we did** — `.github/workflows/ci.yml` builds on Linux/Windows
  cross/macOS cross. It does not run `headless_sim`.
- **Why** — Headless test currently produces no exit code on
  regression. Wiring it into CI without that is just running it for
  show.
- **Revisit when** — `headless_sim` gets assertions.

---

## Map balance + bot AI (M6 P04)

### M5 map geometry deviates from `documents/m5/07-maps.md` for 4 of 8 maps

- **What we did** — During the M6 P04 bake-test pass we edited
  `tools/cook_maps/cook_maps.c` for Concourse, Catwalk, Citadel, and
  Crossfire. The M5 doc still describes the **original design
  intent** (partition-wall wings on Concourse, no-ground-route
  vertical TDM on Catwalk, 200×100 castle dungeons on Citadel,
  180×85 mirror-CTF arena on Crossfire). The shipped geometry is
  smaller / more open / more bot-traversable. Cross-link in
  `documents/m5/07-maps.md` points to `documents/m6/04-map-balance.md`
  for the post-iteration state.
- **Why** — The original layouts produced 0 kills / 0 fires under
  bake-test at every tier (bot-AI couldn't traverse them; the same
  geometry would feel rough to lower-skill humans). Updating the
  M5 doc would lose the design history; updating only the M6 doc
  preserves both the intent and the iteration record.
- **Revisit when** —
  - Bot AI gains enough strength (or the M6+ trained-policy tier
    ships) that the original geometry becomes viable, at which
    point we can revert one or more maps and update both docs.
  - Real-player playtest reports the post-iteration geometry feels
    too forgiving / less interesting than the original. Citadel is
    the most likely candidate — the 200×100 size had real "this
    is the XL map" character that 160×80 trades for accessibility.
  - A designer wants to rework one of the four maps through the
    editor instead of `cook_maps.c`. Either re-cook a 1:1 replica
    of the cook_maps output and edit from there, OR re-cook from
    the M5 spec and re-iterate.

### Mass Driver matrix-dominance retired by Phase 6 (M6 P05)

- **What we did at M6 P04** — left Mass Driver unchanged on the
  argument that one bake seed at one tier isn't a real signal.
- **What changed at M6 P05** — Phase 6 of `documents/m6/05-bot-ai-
  improvements.md` applied option C from the plan: fire_rate_sec
  1.10 → 1.25 (-12 %), aoe_radius 160 → 140 (-13 %). Iter9 30-sec
  matrix at Champion: MD dropped 76 → 66 kills, closer to the
  55-kill median (Plasma Cannon is now top at 75, Rail Cannon next
  at 57).
- **Why kept in TRADE_OFFS** — bot-derived tuning is still a *signal*
  not a verdict, and we tuned anyway. If real-player playtest reports
  "Mass Driver feels underpowered" we should revert. Per the plan's
  Open Question 4, this was a guess between options A/B/C; revert is
  cheap (two numbers in `src/weapons.c`).

### Bot opportunistic-fire scan radius is `1.6 × awareness`, a magic constant

- **What we did** — `src/bot.c::bot_step` post-tactic adds an
  opportunistic-fire check: if the strategy goal didn't already
  set `wants.want_fire`, scan for the nearest LOS-clear enemy
  within `1.6 × mind->pers.awareness_radius_px` (with a floor of
  1200 px) and aim+fire on it. Without this fallback the maps
  with sparse central engagement (Concourse, Catwalk, Citadel)
  produced 0 fires regardless of tier.
- **Why** — The full strategy/tactic pipeline IS the answer; the
  opportunistic-fire fallback is a backstop for cases where the
  strategy correctly chose REPOSITION / PURSUE_PICKUP but an enemy
  happens to walk into LOS during the move. A real player fires
  on visible enemies regardless of their current "plan"; bots
  should too. The 1.6× multiplier + 1200 px floor are tuned to the
  current map sizes — at 160×80 (Citadel) the floor barely matters;
  at 100×40 (Foundry) the 1.6× factor extends Veteran's 900 px
  awareness to 1440 px which still doesn't reach across the map.
- **Revisit when** —
  - Map sizes change significantly (smaller → reduce constants;
    larger → consider per-tier scaling instead of fixed multiplier).
  - The strategy scorer learns to keep score_engage > 0 even when
    LOS is briefly blocked — that would make the fallback
    redundant.
  - A balance pass on tier `awareness_radius_px` values changes the
    relative ratios; the fallback should track tier intent.

### Riot Cannon is bot-hostile (matrix kills: 10 vs. 76 for Mass Driver)

- **What we did** — Across the 320-cell loadout matrix, Riot Cannon
  posted 10 kills against 48 329 fires (4 833 pellets per kill,
  10× worse than the next-worst weapon). The bot fires it whenever
  the strategy goal allows; the pellets just don't connect.
- **Why** — Riot Cannon fires 6 pellets in a `spread_cone_rad`
  cone. The bot's aim direction is a single point per fire; the
  cone disperses pellets in 6 directions of which 1 is the
  bot's intended aim. At mid-to-long range the pellets miss in
  the other 5 directions. The fix is to either (a) gate Riot
  Cannon engagement to `weapon.range_px` distances only (close-
  range), or (b) have the bot's aim model dither the per-pellet
  aim points so multiple pellets converge on the target. Both
  changes touch the bot's strategy/tactic layer; both are real
  work. We elected to ship the imbalance as findings rather than
  fix it this pass.
- **Revisit when** —
  - The bot AI gets a per-weapon engagement-range gate (the obvious
    fix lives at the strategy layer: `score_engage` returns 0 when
    the enemy is outside the weapon's effective range).
  - We add a "spread weapons aim mode" to the motor — instead of
    aiming AT the chest, aim a small cone width offset from chest
    so the cone overlaps the target volume.
  - A second balance pass on Riot Cannon's per-pellet damage
    compensates: bumping damage to ~30 per pellet would let the
    handful of connecting pellets matter even with the bot's
    spray-and-pray aim.

### Bot jet fuel lockout is a hard 10 % / 40 % hysteresis

- **What we did** — `src/bot.c`'s motor adds a `jet_locked_out`
  flag per BotMind. Falls below `fuel_frac = 0.10` → locked.
  Climbs back to `0.40` → cleared. While locked, BTN_JET is
  suppressed entirely, even when the strategy / tactic wants it.
- **Why** — Without this, bots mashed JET against an empty tank
  in tight corners: `apply_jet_force` gates on fuel internally, so
  the input flowed but no thrust applied; the bot looked stuck
  with JET held. The 10/40 band is wide enough to keep the gauge
  flutter (press → empty → release → 1-tick regen → press) off
  the wire, but tight enough that bots return to normal jet use
  within a couple seconds of grounding. Chassis-specific
  `fuel_regen` (Scout > Trooper > Heavy) drives the timing
  naturally — Scouts wait ~1.2 s, Heavies wait ~2.5 s, just by the
  physics. The numbers are guesses, not playtested.
- **Revisit when** —
  - Tier-aware fuel management — Champion bots might be expected
    to time their JET pulses to last 90 % rather than the
    coarse "let it run dry then wait." A real player feathers JET
    to avoid the lockout entirely.
  - A specific jetpack variant (`JET_GLIDE_WING` has lift at
    fuel=0; `JET_JUMP_JET` doesn't use JET at all) needs a
    different lockout shape. Today all four jetpack types share
    the same gate; the deficit is small at the bot's skill ceiling.
  - Real playtest reports bots "obviously refilling" in a way
    that reads as inhuman. Tighten the 40 % threshold, or
    randomize per-bot to break the chassis-determined cadence.

### Bot map vote is uniform-random across the 3 cards

- **What we did** — `lobby_vote_cast_bots` casts one vote per bot
  slot, choice picked by `pcg32_next(rng) % 3`. The world RNG is
  the seed source; the choice is uniform.
- **Why** — Bots have no map preferences (no "Mass Driver
  rewards" model of map awareness in v1). A uniform random keeps
  the vote tally noisy in a way that doesn't bias the rotation:
  with 4 bots and 1 human, the human still has effective
  pick-power over their preferred card (the bots smear the other
  two). A weighted vote — e.g. "bots prefer maps that suit their
  chassis" — would be design-deep work and is wrong for v1: bot
  AI doesn't yet pick a chassis preference.
- **Revisit when** —
  - Bots gain a "match-history weight" — preferring maps they
    haven't played recently — to encourage rotation variety.
  - A specific map is consistently picked by bots in a way that
    irritates human players (e.g. CTF maps get over-represented
    because the post-round vote runs even when the next mode is
    FFA — but the vote cards are mode-filtered, so this scenario
    can't actually happen today).

### `NET_MSG_LOBBY_ADD_BOT` is additive — no protocol bump

- **What we did** — Added wire message id 35 (`LOBBY_ADD_BOT`,
  1-byte body = tier). Server-side handler validates `is_host`
  and applies. Did not bump the protocol id (currently `S0LK`).
- **Why** — Pure additive change: an old client connecting to a
  new server doesn't break because the old client never sends
  the new message id. A new client connecting to an old server
  hits the server's default-discard branch — the bot just
  doesn't appear, no crash, no desync. The bump-or-not call is
  judgment; we picked not-bump because the failure mode is
  "Add Bot silently no-ops" which is exactly what we'd want
  during a version skew.
- **Revisit when** —
  - The wire format changes in a way that's NOT purely additive
    (e.g. extending an existing message body). At that point
    bump the protocol id and add this entry's note to the bump's
    list of "what changed."
  - We start versioning the protocol — assigning a semantic
    version, not just a 4-byte tag. Then each additive change
    can bump a minor version without forcing a major.

---

## Bot AI improvements (M6 P05)

### Bot nav arc-aware JET reach is a 2-segment ray approximation

- **What we did** — `jet_arc_clear()` in `src/bot.c` tests JET reach
  feasibility by casting two straight-line rays through an iterative
  midpoint peak — 200 px above the higher endpoint, walking up in
  100-px steps to 800 px. If any peak height produces clear rays in
  both directions, the JET reach is added. We don't simulate the
  actual parabolic arc that a jetting bot would fly.
- **Why** — A true parabolic feasibility check would need iterative
  trajectory integration with mech mass + jet thrust + gravity at
  the build step. That's complex code we'd run once per pair of nav
  nodes (~50 k pair tests on Citadel). The two-segment approximation
  has the same algorithmic shape as the floor-corridor check and
  costs ~7 ray casts in the worst case. False positives (claimed
  JET reach the bot can't physically fly) are caught by the
  stuck-detector flipping the bot off after 4 s; false negatives
  (rejected JET reach the bot COULD fly) are absorbed by the
  `bfs_closest_reachable` fallback.
- **Revisit when** —
  - A specific map's bots are visibly stuck cycling between two
    nodes connected by a JET edge that arc-clear claims is feasible.
    Add a `BOT_NAV_DUMP=1` re-run and inspect the rejected edges.
  - We add a "bot replay debugger" that records bot inputs and lets
    us scrub through a stuck moment; that would let us validate
    arc-clear vs. physical trajectory empirically.

### 1v1 PASS rate is 26/32 cells (was 32/32 target)

- **What we did** — Iterated the 8 cook_maps to make 1v1 bot play work:
  pillar archway on Reactor, partition passage on Citadel, elevated
  ramps everywhere (so floor walks pass beneath them), shorter cover
  stubs on Crossfire. Added JET-assist in the bot motor (pulses
  BTN_JET on upward path hops ≥ 24 px). After iteration: 26 of 32
  (tier × map) cells produce > 0 fires in 60-s 1v1 bake (was 11/32
  in the M6 P04 baseline — 2.4× improvement). All 8 maps pass at
  Elite + Champion; 6 maps pass at every tier.
- **Cells that still fail (6 of 32)**:
    - **Foundry Recruit + Veteran** — Foundry's mid-map cover wall
      (5 tiles tall, cols 49-50) blocks low-tier bots; only Elite +
      Champion's wider awareness sees past it.
    - **Concourse Recruit + Veteran** — Same pattern: 100×60 atrium
      is wider than the 500/900 px low-tier awareness radii.
    - **Aurora Elite + Champion** — Phase 2's engagement-node picker
      consistently chooses an unreachable mountain peak; the bots
      try to JET there and stall.
    - **Crossfire Champion** — Phase 2 picks a high engagement node
      (rail-cannon perch above sky bridge) that needs sustained JET
      the chassis can't deliver in one fuel cycle.
- **Why** — The remaining failures are tier-specific awareness
  artifacts (Recruit/Veteran can't see across wide maps) and
  engagement-picker overshoot (Elite/Champion's larger awareness
  picks aggressive targets, sometimes inside geometry the bot can't
  reach). They're orthogonal to nav topology; the maps themselves
  are correctly traversable for the average tier.
- **Revisit when** —
  - User reports specific 1v1 stuck moments. Run with `BOT_TRACE=1`
    on the failing map+tier to see what node the bot is targeting.
  - We add a "reachability filter" to Phase 2's `nav_pick_position`
    so the engagement node must be in the bot's connected component
    via A* (currently the geometric/visibility filter only).
  - Low-tier awareness scaling proves an issue in playtest; we may
    want to bump Recruit awareness 500 → 700 if humans miss them
    engaging.

### Weapon engagement profile numbers are guesses, not data-derived

- **What we did** — `g_weapon_profiles[WEAPON_COUNT]` in `src/bot.c`
  ships with hand-picked optimal_range_px / effective_range_px /
  ideal_strafe_px / prefers_high values. The plan §4.3 suggested
  calibrating against `documents/04-combat.md` ranges and the iter7
  matrix's measured fires-per-kill; we used the doc ranges as a
  starting point but didn't iterate.
- **Why** — The Phase 6 Mass Driver retune produced one matrix delta
  (76 → 66 kills); a full per-weapon calibration would need 8
  iterations of the matrix sweep, each running 320 cells × 60s ×
  Champion = ~5h of bake compute. That's a separate doc/PR.
- **Revisit when** —
  - A matrix sweep shows any weapon's kill total drifting more than
    ±15 % from the median.
  - Real-player playtest reports a weapon "feels wrong at X range"
    that disagrees with the bot's behavior at that range.
  - We want the bot to teach players the weapon's optimal range
    (an Elite bot strafing at 1100 px with Rail Cannon should LOOK
    like a sniper, not a brawler).

### Bot retreat doesn't deny ENGAGE; it damps the score

- **What we did** — When `bot_aggression < 0.30`, score_engage gets
  multiplied by 0.2× (and score_retreat gets boosted to 0.95 ×
  (1 - agg/threshold)). The damping is per-tick — if engage at full
  score was 0.6, the post-damp is 0.12; retreat at full is 0.95. So
  retreat wins, usually. But a Heavy with full ammo + an LOS-clear
  enemy at 200 px can still produce engage * 0.2 = ~0.18, which
  beats retreat * (1 - 0.30/0.30) = 0 if HP hovers RIGHT at the
  threshold.
- **Why** — A hard "ENGAGE prohibited when hurt" rule made bots feel
  passive in early Phase 4 prototypes. The 0.2× damp lets a bot in
  desperation still trade shots — which is what a real player at
  10 % HP with a primed weapon would do. The fallback is that
  opportunistic-fire fires anyway if an enemy is LOS-clear within
  awareness, so the bot is never silent when an enemy walks up to
  it during retreat.
- **Revisit when** —
  - User reports "Recruit bots ignore the retreat threshold" — that's
    the personality.aggression baseline (set to 0.30 for Recruit) +
    the 0.2× damp, which when multiplied with the tier baseline
    leaves the engage score competitive. Trade-off explicitly: the
    Veteran/Elite/Champion bots retreat cleanly; Recruit was
    designed not to retreat at all anyway (retreat_threshold = 0).
  - We add a kill-feed-event-driven aggression boost — bot just got
    a kill, briefly inflate aggression so the bot pushes the next
    enemy. Currently aggression only DECAYS toward 1.0 over 2 s
    after damage.

---

## How to use this file

When you're about to add an "I'll just hack this in for now" — write the
entry here *first*. The discipline of stating "what" + "why" + "when to
revisit" forces you to decide whether it's a real trade-off (worth
keeping) or a code smell that should be fixed before commit.

When a trade-off graduates to a real fix, **delete the entry** rather
than marking it done. This file is a queue of debt, not a changelog.
