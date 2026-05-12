# M5 — Controls and residual fixes

Two related concerns picked up after the per-chassis / per-weapon distinctness pass forced a controls re-think:

1. **Secondary-weapon usage** — the M4 build only fires the secondary slot after a `BTN_SWAP`. With visibly distinct weapons (per [12-rigging-and-damage.md](12-rigging-and-damage.md) §"Per-weapon visible art"), having to swap-then-fire to throw a grenade feels clumsy. We add **`BTN_FIRE_SECONDARY` (RMB)** that fires the secondary slot one-shot without changing the active weapon. Standard modern-FPS pattern.

2. **A trade-off sweep** — the canonical [TRADE_OFFS.md](../../TRADE_OFFS.md) has accumulated entries since M1. Many are resolved by other M5 sub-docs; some are forced into resolution by interactions with M5's new systems (slope physics, damage feedback, weapon visuals); some are explicitly left alone. This doc enumerates which is which so the planning is honest.

The keybind table, the new fire path, and the trade-off triage are all small individually. Together they're the polish/honesty layer that closes M5.

## BTN_FIRE_SECONDARY — the one new control

### What it does

Press RMB → fires the **secondary slot** for one shot, regardless of which slot is currently active. Active slot is unchanged; the weapon you were holding stays held. The fire path uses the secondary's stats (damage, fire rate, cooldown, ammo, recoil, bink).

Examples:

- Trooper with Pulse Rifle (primary) + Frag Grenades (secondary). LMB rapid-fires the rifle. RMB throws a grenade. The rifle is still active; you keep firing it after the throw.
- Sniper with Rail Cannon + Sidearm. LMB charges + fires the rail. RMB pops a Sidearm round at someone closing distance, without losing the rail's charge.
- Heavy with Mass Driver + Combat Knife. LMB fires the rocket. RMB melees if anyone gets too close (the knife range is 60 px).
- Engineer with Auto-Cannon + Frag Grenades. RMB throws grenades while the rifle stays active.

The shared **fire cooldown** (`mech.fire_cooldown`) still applies — you can't fire LMB and RMB on the same tick, and either weapon's cycle locks both buttons until the cooldown clears. This prevents the "double DPS by alternating buttons" exploit. The secondary's fire-rate-sec is what gates the next RMB shot; the primary's gates the next LMB; the larger of the two gates anything in between.

### Why not just bind RMB to "swap and fire"

Considered and rejected. Two ticks (swap, then fire) means RMB is delayed by two frames vs. one-shot direct fire. The visible result is either a 33 ms delay or a swap-flicker — both bad. Direct fire is cleaner code and feels better.

### Why not bind RMB to alt-fire on the primary

Some games (CS:GO, Halo) use RMB for alt-fire/scope on the primary. Cool but not what we ship — we don't have alt-fire weapons (no scope mechanic, no underbarrel grenades). RMB-as-quick-secondary maps onto our existing two-slot loadout and gives every loadout combo a meaningful RMB.

### Updated input bitmask

```c
// src/input.h — additions
enum {
    BTN_LEFT     = 1u << 0,
    BTN_RIGHT    = 1u << 1,
    BTN_JUMP     = 1u << 2,
    BTN_JET      = 1u << 3,
    BTN_CROUCH   = 1u << 4,
    BTN_PRONE    = 1u << 5,
    BTN_FIRE     = 1u << 6,    /* primary fire — fires active slot */
    BTN_RELOAD   = 1u << 7,
    BTN_MELEE    = 1u << 8,
    BTN_USE      = 1u << 9,
    BTN_SWAP     = 1u << 10,
    BTN_DASH     = 1u << 11,
    BTN_FIRE_SECONDARY = 1u << 12,   /* NEW — fires secondary slot one-shot */
    /* 13..15 reserved */
};
```

`BTN_FIRE_SECONDARY` sits in bit 12 (the previously-reserved range, per the existing comment "12..15 reserved for future binds").

The 16-bit `buttons` field already covers it; no snapshot wire format change. The `prev_buttons` edge-detect path in `mech_step_drive` automatically picks up the new bit.

### Implementation in `mech_try_fire`

```c
// src/mech.c::mech_try_fire — added branch
uint16_t pressed = (uint16_t)((~m->prev_buttons) & in.buttons);

/* Existing path: BTN_FIRE on the active slot. */
if (in.buttons & BTN_FIRE) {
    fire_active_slot(w, mid);   // existing code, unchanged
}

/* NEW path: BTN_FIRE_SECONDARY on the OTHER slot. */
if (pressed & BTN_FIRE_SECONDARY) {
    fire_other_slot_one_shot(w, mid);
}
```

`fire_other_slot_one_shot` factors out the fire-dispatch into a function that takes a weapon_id explicitly:

```c
static void fire_other_slot_one_shot(World *w, int mid) {
    Mech *m = &w->mechs[mid];
    int other_slot = m->active_slot ^ 1;
    int weapon_id  = (other_slot == 0) ? m->primary_id : m->secondary_id;
    int *ammo_ptr  = (other_slot == 0) ? &m->ammo_primary : &m->ammo_secondary;

    const Weapon *wpn = weapon_def(weapon_id);
    if (!wpn) return;
    if (m->fire_cooldown > 0.0f) return;     // shared cooldown still gates
    if (m->reload_timer > 0.0f) return;
    if (wpn->mag_size > 0 && *ammo_ptr <= 0) return;

    /* Save active state, swap to other slot, fire, restore. */
    int saved_w = m->weapon_id;
    int saved_a = m->ammo;
    int saved_am = m->ammo_max;

    m->weapon_id = weapon_id;
    m->ammo      = *ammo_ptr;
    m->ammo_max  = wpn->mag_size;

    /* Dispatch by fire kind — same switch as fire_active_slot. */
    weapons_fire_dispatch(w, mid, wpn);

    /* Decrement the OTHER slot's ammo from the temporary state. */
    *ammo_ptr = m->ammo;

    /* Restore active state. */
    m->weapon_id = saved_w;
    m->ammo      = saved_a;
    m->ammo_max  = saved_am;
}
```

Cost: ~50 LOC including the dispatch refactor. The renderer's per-tick "show muzzle flash on the secondary's visible position" path uses the same `weapon_sprite_def` from [12-rigging-and-damage.md](12-rigging-and-damage.md) to flash the right weapon — for the one frame of the RMB fire, the renderer paints the secondary weapon at R_HAND in addition to (in front of) the primary.

### Updated keybind table (player-facing)

Default keybinds shipped at M5:

| Action | Key | Notes |
|---|---|---|
| Move left | A | |
| Move right | D | |
| Jump | Space | |
| Jet | W or Right Mouse Hold | (LMB-RMB-down combo can scoop both fire + jet — accepted) |
| Crouch | Ctrl | Drops through ONE_WAY platforms |
| Prone | X | |
| **Primary fire** | **Left Mouse** | Fires active slot |
| **Secondary fire** | **Right Mouse** | Fires inactive slot one-shot — **NEW at M5** |
| Reload | R | |
| Melee | F | |
| Use / interact | E | Engineer ability, grapple release, flag interact |
| Swap weapon | Q | Toggle active slot |
| Dash | Shift | Scout dash, Burst-jet boost |

Note the conflict: the previous design had RMB un-bound (or unspecified). M5 explicitly binds RMB to BTN_FIRE_SECONDARY. The "RMB hold = jet" pattern is an alternate keybind from some competitor games — not our default. Players who rebind manually can still set RMB → BTN_JET, but then they lose the secondary-fire shortcut.

The platform layer in `src/platform.c` writes the bitmask:

```c
// src/platform.c — added
if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) buttons |= BTN_FIRE_SECONDARY;
```

A keybinds config file (`soldut_controls.cfg`) is M6 polish; M5 ships hard-coded defaults.

### What this DOESN'T do

- **Doesn't enable simultaneous LMB+RMB fire.** The shared `fire_cooldown` keeps the rate-of-fire honest.
- **Doesn't allow BTN_FIRE_SECONDARY to charge** (Rail Cannon's `charge_sec`). RMB-fires the secondary as a one-shot release-on-press; charge weapons in the secondary slot still need the active-slot path (BTN_FIRE while active=secondary). Frag Grenades, Sidearm, Burst SMG, Micro-Rockets, Combat Knife, Grappling Hook — none of these charge — so this matches the realistic loadouts.
- **Doesn't change the existing BTN_SWAP flow.** Press Q to toggle the active slot; LMB fires whatever's active; RMB fires the other one. Three input verbs for two weapons. Standard.

## Trade-offs sweep

[TRADE_OFFS.md](../../TRADE_OFFS.md) is the project's queue of debt. Per the CLAUDE.md instructions: when a trade-off graduates to a real fix, **delete the entry** rather than mark it done. The sweep below identifies which entries M5 resolves (deletion candidates), which M5 forces into resolution (new pickups), and which we explicitly leave alone.

### Resolved by M5 — these entries get deleted on land

> **Status as of P16 + wan-fixes**: every row below has been deleted
> from `TRADE_OFFS.md` as the implementing prompt landed — *except*
> "Hard-coded tutorial map". P01 shipped the `.lvl` loader but the
> entry's deletion gate is the authored maps themselves (P17/P18); the
> entry stays open until the first authored `.lvl` retires
> `level_build_tutorial`. The "Mechs rendered as raw capsules" entry's
> own gate slipped from P12 → P15/P16 (waiting on chassis PNGs); it
> was finally deleted at P16 when all 5 chassis atlases shipped via
> the gostek extraction path.

| Trade-off entry | Resolved by | Notes |
|---|---|---|
| Mechs rendered as raw capsules | [08-rendering.md](08-rendering.md), [12-rigging-and-damage.md](12-rigging-and-damage.md) | Sprite atlas runtime lands at P10; capsule fallback intentionally kept until P12 (damage feedback) makes the sprite path canonical. **P12 shipped damage feedback in BOTH render paths (hit-flash + decals + spray + emitter + smoke), but no `assets/sprites/<chassis>.png` files exist on disk until P15/P16 — so capsule fallback is what fires in real play through P14 development. Per the post-P12 audit in `TRADE_OFFS.md`, the entry-deletion gate moved from P12 to P15/P16 (asset generation).** |
| Only a tile grid; no per-tile polygons | [03-collision-polygons.md](03-collision-polygons.md) | Free-poly broadphase + slope physics. |
| Hard-coded tutorial map | [01-lvl-format.md](01-lvl-format.md), [07-maps.md](07-maps.md) | `.lvl` loader + 8 authored maps shipped (P17/P18 via `tools/cook_maps`). **Entry stays open** — `shotmode.c` + `tests/headless_sim.c` still call `level_build_tutorial` directly. Deletion gate is migrating those callsites to `level_load` against a checked-in `.lvl` fixture. |
| Grappling Hook is a stub | [05-grapple.md](05-grapple.md) | Full anchor + retract + release. |
| Engineer ability heals self instead of dropping a deployable | [04-pickups.md](04-pickups.md) | `BTN_USE` drops `PICKUP_REPAIR_PACK`. |
| Burst SMG fires all rounds on the same tick | [04-pickups.md](04-pickups.md) §"Residual fixes" | `burst_pending_*` queued cadence. |
| Default raylib font | [08-rendering.md](08-rendering.md) §"TTF font" + [11-art-direction.md](11-art-direction.md) §"Fonts" | Atkinson + VG5000 + Steps Mono. |
| Map vote picker UI is partial | [07-maps.md](07-maps.md) §"Vote picker UI" | Three-card modal in summary screen. |
| Kick / ban UI not exposed | [07-maps.md](07-maps.md) §"Host controls" | Row-hover buttons + confirm modals. |
| `bans.txt` not persisted | [07-maps.md](07-maps.md) §"Host controls" | Load on start, write on update. |
| Single-player mode has no practice dummy | [04-pickups.md](04-pickups.md) §"Practice dummy" | New `PICKUP_PRACTICE_DUMMY` kind. |
| Loadouts ship per-mech but no lobby UI | (already resolved at M4) | Listed for completeness. |

### Picked up at M5 — new fixes M5 makes

These weren't in the M5 roadmap originally but are forced into scope by interaction with M5's new systems. We address them here.

#### A. Slope-aware post-physics anchor

The M1 fix `mech_post_physics_anchor()` zeroes `prev_y = pos_y` for the upper body when `m->grounded && (anim_id == ANIM_STAND || ANIM_RUN)`. This was load-bearing at M1 (gravity sag accumulated faster than the constraint solver corrected it).

**The new slope physics in [03-collision-polygons.md](03-collision-polygons.md) breaks this**. On a 60° slope with no run input, the body should slide passively downhill — but the post-physics anchor zeros Y-velocity every tick, killing the slide. The slope feel doesn't work.

**Fix**: gate the anchor on **flat contact only**. When the foot's contact normal `ny < -0.92` (within ~22° of straight up), run the anchor as M1 does. Otherwise (on a slope), skip the anchor entirely; let the slope-tangent run velocity + slope-aware friction handle pose maintenance through the regular physics.

```c
// src/mech.c::mech_post_physics_anchor — gate on flat contact
ParticlePool *p = &w->particles;
int lf = m->particle_base + PART_L_FOOT;
int rf = m->particle_base + PART_R_FOOT;
float ny_l = p->contact_ny_q[lf] / 127.0f;
float ny_r = p->contact_ny_q[rf] / 127.0f;
float ny_avg = (ny_l + ny_r) * 0.5f;
if (ny_avg > -0.92f) return;       // sloped — skip the anchor, let slope physics work
/* ... existing anchor body ... */
```

Cost: 6 LOC. Verifying the fix: a mech standing on a 30° slope without input should slowly slide down (driven by gravity-along-tangent + low friction). On a 5° slope it should hold station (high friction). On flat ground it should hold rigidly (anchor fires).

This fix is updated into [03-collision-polygons.md](03-collision-polygons.md) §"Slope-aware post-physics anchor".

#### B. Render-side interpolation alpha + fixed-step accumulator

Two M1 trade-offs (`60 Hz simulation, not 120 Hz`, `No render-to-sim interpolation alpha`, `Vsync / frame-pacing leak`) share the same root cause: the M1 build calls `simulate()` once per render frame, with `dt = wall-clock since last render`. When vsync-fast (small windows, fullscreen on a 144Hz display), simulate runs faster than 60 Hz, and per-tick caps don't scale.

The existing M5 damage-feedback layers (hit-flash tint at 80–120 ms, smoke from damaged limbs at 8-tick cadence) and the new slope physics make this gap visible. **Hit-flash that pops because of a snap-on-snapshot is worse than no flash.**

**Fix**: implement the canonical Glenn Fiedler accumulator + render-side alpha. Sim runs at fixed 60 Hz inside `main.c`'s loop; render reads `accum / TICK_DT` as `alpha` and lerps every visible body (mech bone positions, projectile positions, FX positions) between previous and current sim states.

The 60-Hz-vs-120-Hz question stays open — we ship 60 Hz at M5 because the slope physics tuning happened against 60 Hz. The accumulator makes 120 Hz a flag-flip in M6.

```c
// src/main.c — already has accum loop, just needs alpha forwarding to render
while (accum >= TICK_DT) {
    simulate_step(&world, ...);
    accum -= TICK_DT;
}
float alpha = (float)(accum / TICK_DT);   // already computed; not currently used
renderer_draw_frame(&rd, &world, sw, sh, alpha, ...);
```

Renderer side:

```c
// src/render.c — extend draw_mech to lerp between prev_pos and current_pos
// (requires storing per-particle prev-frame snapshot; ~100 LOC)
Vec2 head = lerp(prev_head, cur_head, alpha);
// ... etc
```

Cost: ~150 LOC across `world.h` (per-particle prev-frame), `simulate.c` (snapshot prev-frame at start of each tick), `render.c` (lerp every read). Performance: one per-particle copy at start of tick (negligible), one lerp per render call. Inside budget.

Resolves: `60 Hz simulation, not 120 Hz`, `No render-to-sim interpolation alpha`, `Vsync / frame-pacing leak`. The 120 Hz toggle remains deferred (just the accumulator infrastructure lands).

This goes into [08-rendering.md](08-rendering.md) §"Render-side interpolation alpha".

#### C. Visual smoothing of reconciliation jumps + snapshot interp upgrade

M2 trade-offs `No client-side visual smoothing of reconciliation jumps` and `Snapshot interp is a 35% lerp, not a full two-snapshot buffer` were tolerated at M2 because LAN-only testing made the snaps invisible. **At M5 they're visible**: a hit-flash that triggers on a snapshot apply with a 9-px snap reads as a glitch, not a hit.

**Fix**: extend the existing `Reconcile.visual_offset` (already computed and decayed at `src/reconcile.c`; the renderer just doesn't read it). The renderer reads `reconcile.visual_offset` for the local mech's draw, applying it as an additive offset to all the mech's particles' rendered positions. The offset decays over ~6 frames (~100 ms). Server snaps that are sub-pixel are invisible; larger snaps smooth out.

For remote mechs, upgrade the 35% lerp to a **two-snapshot buffer**: keep the last 3 snapshots per mech in a ring; render from `now - 100 ms`, interpolating between the bracket pair. This is the spec in [05-networking.md](../05-networking.md) §"Snapshot interpolation"; M5 finally implements it.

Cost: ~200 LOC in `snapshot.c` + ~50 LOC in `render.c`. Inside the M5 schedule.

Resolves: `No client-side visual smoothing of reconciliation jumps`, `Snapshot interp is a 35% lerp, not a full two-snapshot buffer`.

#### D. `is_dummy` bit on the wire

M4 trade-off `Dummy is a non-dummy on the client` — the practice dummy spawned at M5 (per [04-pickups.md](04-pickups.md) §"Practice dummy") spawns as a real-mech entity on remote clients. We have one bit free in the EntitySnapshot's `state_bits` field after the M5 widening (covered in [04-pickups.md](04-pickups.md) for powerup state). Spend that bit:

```c
enum {
    SNAP_STATE_IS_DUMMY = 1u << 11,    // NEW; sits in the M5-widened state_bits u16
};
```

Client uses this bit to skip the right-arm aim drive on dummies (mirrors the server's `if (!m->is_dummy)` check at `src/mech.c::build_pose`).

Cost: 4 LOC. Resolves the trade-off.

### Deferred past M5 — entries that stay

These remain tolerated. Not load-bearing for the M5 ship.

| Trade-off | Why we keep it |
|---|---|
| `Post-physics kinematic anchor for standing pose` | The anchor itself stays — only the slope-gating is added. Replacement (PBD/XPBD) is a one-week refactor; deferred. |
| `No angle constraints in active use` | Tolerated; ragdoll behavior is acceptable. Stretch for M6. |
| `120 Hz sim` | Accumulator lands at M5 (so 120 Hz is a flag-flip). The actual rate stays 60 because slope-physics tuning happened against it. Decide after playtest. |
| `Non-cryptographic handshake token (keyed FNV1a, not HMAC-SHA256)` | LAN-first; defer until WAN-public. |
| `No snapshot delta encoding` | Bandwidth fits. Even with the M5 EntitySnapshot widening (powerups + grapple state) we're at ~30 KB/s downstream. Below the 80 kbps target. |
| `No server-side entity culling` | Defer until 16+ player playtest. |
| `Projectile vs bone collision is sample-based, not analytic` | Tolerated. Sample-based is fine at M5 weapon speeds. |
| `No cone bink — just per-shot self-bink + accumulator` | Tolerated. Per-projectile-tick proximity check is M6 polish. |
| `Shot-mode driving of UI screens is not built` | Tolerated. Network test scaffold catches functional bugs; UI screenshots are nice-to-have. |
| `tick_hz config field accepted but ignored` | Becomes meaningful once 120 Hz is on the table. Defer with the rate decision. |
| `headless_sim is the only physics test` | Not blocking the ship. CI for physics correctness is M6. |
| `No CI for physics correctness` | Bound to the headless_sim assertions item. Defer with it. |

### Trade-offs M5 *introduces*

For honesty: M5 lands several deliberate compromises that should go straight into [TRADE_OFFS.md](../../TRADE_OFFS.md) as new entries when the work ships. Already pre-disclosed in the relevant sub-docs:

- **`.lvl` v1 = lock-in** (per [01-lvl-format.md](01-lvl-format.md)) — bumping versions costs editor + loader churn. *Landed at P01.*
- **Renderer draws polygons as flat triangles** (P02 stopgap; replaced by sprite atlases at P13). *Landed at P02.*
- **Slope test bed is hardcoded in `level_build_tutorial`** (removed when authored maps land at P17). *Landed at P02.*
- **Slope-physics tuning numbers are starting values** (per [03-collision-polygons.md](03-collision-polygons.md); tuned after authored maps land). *Landed at P02.*
- **Editor undo is whole-tile-grid snapshot, not differential** (per [02-level-editor.md](02-level-editor.md)).
- **Pickup transient state isn't persisted across host restarts** (per [04-pickups.md](04-pickups.md)).
- **Grapple anchor uses server-current position (no lag comp)** (per [05-grapple.md](05-grapple.md)).
- **Carrier secondary is fully disabled, not partially** (per [06-ctf.md](06-ctf.md)).
- **No animated mech sprites** (per [11-art-direction.md](11-art-direction.md)).
- **Halftone post is a fragment shader, not per-asset texture work** (per [11-art-direction.md](11-art-direction.md)).
- **AI-assisted with substantial post-generation editing** is the disclosure; commercial output OK but pure AI uncopyrightable (per [11-art-direction.md](11-art-direction.md) §"License hygiene").
- **Whole-mech hit flash, not per-particle** (per [12-rigging-and-damage.md](12-rigging-and-damage.md)).
- **Decal-overlay damage, not sprite-swap damage states** (per [12-rigging-and-damage.md](12-rigging-and-damage.md)).
- **Symmetric mech parts to avoid flip-past-180° artifact** (per [12-rigging-and-damage.md](12-rigging-and-damage.md)).
- **No re-sort by Y mid-tumble** (per [12-rigging-and-damage.md](12-rigging-and-damage.md)).
- **L_HAND dangles for both one-handed AND two-handed weapons** (per [12-rigging-and-damage.md](12-rigging-and-damage.md) and `TRADE_OFFS.md` "Left hand has no pose target"). The two-handed foregrip-pose driver was attempted at P11 in three variants (strength-0.6 L_HAND yank, clamped, snap-pose-IK strength-1.0) and reverted post-ship — all three drifted the body in the aim direction during steady-state hold because the L_ARM rest-state decouples from L_SHOULDER during the constraint-solve iterations as PELVIS shifts under R_ARM aim drive. Real fix is a 2-bone IK constraint INSIDE the solver loop; deferred to M6. `WeaponSpriteDef.pivot_foregrip` stays on the table so the future IK consumer can plug in without sprite-def churn.

When M5 ships, the planner walks this list, opens a TRADE_OFFS entry per item still unresolved, and *deletes* every "resolved" entry from §"Resolved by M5".

## Done when

- `BTN_FIRE_SECONDARY = 1u << 12` defined in `src/input.h`.
- Platform layer maps RMB → `BTN_FIRE_SECONDARY` in `src/platform.c`.
- `mech_try_fire` has the `fire_other_slot_one_shot` branch; ammo decrements from the correct slot; recoil + bink + cooldown all apply correctly to the secondary.
- A Trooper with Pulse Rifle + Frag Grenades plays as: LMB rifle fire, RMB grenade throw, swap-back is implicit. Verified in shot test (`tests/shots/m5_secondary_fire.shot`).
- Slope-aware post-physics anchor: a mech standing on a 60° slope with no input slides downhill; on a 5° slope it holds station.
- Render-side interpolation alpha lands; visible result is no jitter when render rate exceeds sim rate (test by running fullscreen on a 144 Hz monitor).
- Reconciliation visual offset is read by the renderer; remote-mech snapshot interp uses a two-snapshot buffer with `~100 ms` lookback.
- `is_dummy` bit rides snapshots; remote dummies don't aim.
- Updated keybind table is in the in-game pause menu / about screen.
- The "Resolved by M5" trade-off entries are deleted from [TRADE_OFFS.md](../../TRADE_OFFS.md).

## What this doc explicitly doesn't cover

- **Keybind remapping UI**. Hard-coded defaults at M5; runtime remapping is M6 polish.
- **Controller / gamepad support**. Out of scope per the design canon.
- **Game-feel tuning of secondary fire** (whether RMB cancels a primary reload, whether RMB shares cooldown with a charged Rail Cannon, etc.). Tuned in playtest; the design above is the starting point.
