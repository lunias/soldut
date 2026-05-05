# M5 — Grappling hook

The Grappling Hook secondary weapon. Currently a stub: `src/weapons.c::g_weapons[WEAPON_GRAPPLING_HOOK]` is registered with `fire = WFIRE_GRAPPLE` and `range_px = 600`, but `mech_try_fire`'s WFIRE_GRAPPLE case only logs `"grapple_attempt (NOT YET IMPLEMENTED)"` and ticks the cooldown.

This doc lands the proper implementation. Roughly one engineer-day of work; pairs with map-design beats that need swing/pull movement (per [07-maps.md](07-maps.md)).

## What the player feels

Press fire (with Grappling Hook as the active secondary):

1. A small projectile launches from the right hand toward the cursor.
2. On hit (tile or mech bone), the rope goes taut at the **hit-time distance** between the firer's pelvis and the anchor.
3. The rope acts as a **one-sided length limit** — slack when the firer is closer than rope length, taut when stretched. The firer hangs as a pendulum, swinging under gravity around the anchor (the **Tarzan** feel). There is no auto-contract; the firer doesn't get pinned against whatever they grappled.
4. The rope length is the hit-time pelvis-to-anchor distance, **clamped to 300 px** so a long-range fire still gives a tight, swingable rope instead of a 600-px line you can't really swing on. If the firer hit something farther than 300 px, they're "outside" the rope at attach and get pulled in to 300 px over a few iterations of the constraint solver.
5. **Hold BTN_JET (W)** while attached to **retract** — the rope shortens at 800 px/s (clamped to a 60 px minimum) and the firer zip-lines toward the anchor. Releasing W stops the retract; the rope length stays where it was.
6. While attached, the player can still aim and shoot.
7. **BTN_USE** releases the rope — the firer disconnects and falls / runs / jets.
8. **BTN_FIRE** while ATTACHED auto-releases the current rope and fires a new head. This is the chain-grapple flow that lets the player swing across a level.
9. Other releases happen on the anchor mech's death (dead anchors stop pulling) and on the firer's own death.

This is the Soldat / Worms tradition. It's pure utility — zero damage — but the right map design makes it transformative for vertical traversal.

> **Earlier draft of this doc described an auto-contract design** (rest
> length ticks down at 800 px/s, clamped to a 80 px minimum; firer
> gets pulled to the anchor and held there). The author shipped that
> at first and a playtester immediately reported "I cannot retract it,
> I end up stuck to whatever I grappled" — so we replaced it with the
> static-length rope above. The auto-contract path is gone from the
> code; this doc reflects the shipping behaviour. If a later milestone
> wants a "zip-line up" style retract, that should be a separate input
> binding (e.g. hold BTN_JET while attached) rather than the default
> rope behaviour.

## Data model

```c
// src/world.h — added to Mech
typedef enum {
    GRAPPLE_IDLE = 0,
    GRAPPLE_FLYING,        // hook head in flight
    GRAPPLE_ATTACHED,      // anchored, pulling
} GrappleState;

typedef struct {
    GrappleState  state;
    Vec2          anchor_pos;            // where the head stuck
    int           anchor_mech;           // -1 if anchored to a tile
    int           anchor_part;           // PART_* if anchor_mech >= 0
    float         rest_length;           // current contracting rest length of the constraint
    int           constraint_idx;        // index in world.constraints; -1 if none
} Grapple;

// World struct adds:
Grapple grapples[MAX_MECHS];
```

The grapple state is per-mech; each mech can have one active grapple at a time. The hook *head* during flight is a special-cased projectile (same pool as everything else, but with a marker that says "this is a grapple head, on hit don't damage, instead set the firer's grapple state").

## Wire format

A grapple's state needs to ride snapshots so remote clients can render the rope correctly. The wire has no per-entity dirty mask, so the gating bit lives in the existing `state_bits` u16 (already on the wire after P03's u8→u16 widening). When set, an 8-byte suffix is appended after the entity's regular fields:

```c
// snapshot.h — added
enum {
    SNAP_STATE_GRAPPLING = 1u << 12,   // bit 12 in EntitySnapshot.state_bits
};

// EntitySnapshot trailing suffix when SNAP_STATE_GRAPPLING is set:
//   grapple_state       : u8   (GRAPPLE_IDLE/FLYING/ATTACHED)
//   grapple_anchor_mech : u8   (0xFF = no mech anchor)
//   grapple_anchor_part : u8   (bone index when anchored to a mech)
//   reserved            : u8
//   grapple_anchor_x_q  : i16  (1 px res, direct (int16_t)anchor.x)
//   grapple_anchor_y_q  : i16
// Total: 8 bytes per active grapple.
```

The base EntitySnapshot stays at 28 bytes (its post-P03 size). Idle entities pay 0 bytes of grapple overhead — the suffix is gated by the bit. Active grapples cost 8 bytes × N grapplers × 30 Hz ≈ 240 B/s per active grapple. With 4 simultaneous active grapples (very unusual): 1 KB/s. Trivial.

The suffix uses 1 px resolution (direct `(int16_t)` cast of the anchor world position) rather than the `quant_pos` 4× sub-pixel factor used by `pos_x_q`. The grapple anchor is a static decoration, not a physics-relevant position; sub-pixel precision wasn't needed.

> Earlier drafts of this doc proposed `SNAP_DIRTY_GRAPPLE = 1u << 11` on a per-entity dirty mask. That mask doesn't exist on the wire, so the gating moved into `state_bits` — same intent (idle = 0 bytes), different mechanism.

## The hook head

The hook head is a projectile, but with `kind = PROJ_GRAPPLE_HEAD`:

```c
// src/world.h — added to ProjectileKind
PROJ_GRAPPLE_HEAD,
```

Behavior in `projectile_step`:

- Same drag/gravity (set by the weapon table — `gravity_scale = 0` for the grapple, so it flies straight; speed = 1200 px/s).
- On tile hit: stick. Set `mech.grapple.state = GRAPPLE_ATTACHED`, `anchor_pos = pos`, `anchor_mech = -1`, attach a contracting distance constraint, despawn the projectile.
- On bone hit: stick to that bone particle. `anchor_mech = mech_id`, `anchor_part = bone_part`. The anchor moves with the bone every tick (we read it fresh, see "Anchor tracking" below). The grappled mech *takes no damage* — this is utility, not a weapon.
- On range exceeded (lifetime expires before hitting anything): release the head and reset the grapple state.

```c
// src/projectile.c — special case
if (p->kind[i] == PROJ_GRAPPLE_HEAD) {
    if (hit_wall || hit_mech >= 0) {
        Mech *firer = &w->mechs[p->owner_mech[i]];
        firer->grapple.state = GRAPPLE_ATTACHED;
        firer->grapple.anchor_pos = (Vec2){p->pos_x[i], p->pos_y[i]};
        firer->grapple.anchor_mech = (hit_mech >= 0) ? hit_mech : -1;
        firer->grapple.anchor_part = (hit_mech >= 0) ? hit_part : -1;
        // Initial rest length = current pelvis-to-anchor distance.
        Vec2 pelv = mech_pelvis_pos(w, p->owner_mech[i]);
        float dx = pelv.x - firer->grapple.anchor_pos.x;
        float dy = pelv.y - firer->grapple.anchor_pos.y;
        firer->grapple.rest_length = sqrtf(dx*dx + dy*dy);
        // Spawn the constraint (see below).
        grapple_attach_constraint(w, p->owner_mech[i]);
        // Despawn head.
        p->alive[i] = 0;
        audio_play_at(SFX_GRAPPLE_HIT, firer->grapple.anchor_pos);
    }
    continue;  // skip damage path
}
```

## Rope mechanism: one-sided length-limit constraint

When attached, we install a one-sided distance limit between the firer's pelvis particle and an "anchor particle." The constraint pulls only when the firer is *farther* than rope length; when closer, the rope is slack (no force). Every relaxation iteration of the existing 12-iter constraint loop enforces the limit. The result is pendulum-style swinging — the firer hangs at rope length under gravity and arcs around the anchor.

Two shapes:

- **Tile anchor** (`anchor_mech == -1`): the anchor is a fixed world point. We use a dedicated `CSTR_FIXED_ANCHOR` kind that holds the anchor in `c->fixed_pos` inline. The solver's `solve_fixed_anchor` early-returns when `d ≤ c->rest` (slack) and pulls the particle in when `d > c->rest`.
- **Mech anchor** (`anchor_mech >= 0`): the anchor is the bone particle of the target mech. We use the existing `CSTR_DISTANCE_LIMIT` shape with `a = grappler_pelvis, b = target_bone_particle, min_len = 0, max_len = rope_length`. Same one-sided behaviour — slack when `d ≤ max_len`, pulls in when stretched. The bone moves with the target each tick automatically (no per-tick update needed).

```c
// src/world.h — constraint kinds at v1
typedef enum {
    CSTR_DISTANCE = 0,
    CSTR_DISTANCE_LIMIT,
    CSTR_ANGLE,
    CSTR_FIXED_ANCHOR,        // P06: end b is a fixed Vec2 stored inline
} ConstraintKind;

typedef struct {
    uint16_t a, b;
    uint16_t c;
    uint8_t  kind;
    uint8_t  active;
    float    rest;            // FIXED_ANCHOR: max length
    float    min_len, max_len;// DISTANCE_LIMIT: min/max length
    float    min_ang, max_ang;
    Vec2     fixed_pos;       // FIXED_ANCHOR: inline anchor
} Constraint;
```

Each tick of the simulation (in `mech_step_drive`), the rest length is **only** mutated when the player holds `BTN_JET` (W) — the player-driven retract that lets them zip-line toward the anchor. Without W held the rope is a static-length pendulum, not a winch. The block also updates the constraint's `fixed_pos` so float drift can't shift the anchor, releases on `BTN_USE`, and releases if the anchor mech dies:

```c
// src/mech.c::mech_step_drive — when grapple is ATTACHED
if (m->grapple.state == GRAPPLE_ATTACHED) {
    /* W-hold retract. Rope shortens at GRAPPLE_RETRACT_PXS while
     * BTN_JET is held; clamped at GRAPPLE_MIN_REST_LEN so a fully-
     * retracted firer doesn't end up inside the anchor tile. */
    if (in.buttons & BTN_JET) {
        m->grapple.rest_length -= GRAPPLE_RETRACT_PXS * dt;
        if (m->grapple.rest_length < GRAPPLE_MIN_REST_LEN)
            m->grapple.rest_length = GRAPPLE_MIN_REST_LEN;
    }
    Constraint *c = &w->constraints.items[m->grapple.constraint_idx];
    c->rest    = m->grapple.rest_length;       // tile anchor solver reads this
    c->max_len = m->grapple.rest_length;       // mech anchor solver reads this
    if (m->grapple.anchor_mech < 0) {
        c->fixed_pos = m->grapple.anchor_pos;  // tile anchor authoritative
    }
    if (m->grapple.anchor_mech >= 0
        && !w->mechs[m->grapple.anchor_mech].alive) {
        mech_grapple_release(w, mid);
    }
    if (pressed & BTN_USE) mech_grapple_release(w, mid);
}
```

Tunables (in `src/mech.c` + `src/projectile.c`):

| Constant                    | Value      | What it does                                                |
|-----------------------------|------------|-------------------------------------------------------------|
| `GRAPPLE_MAX_REST_LEN`      | 300 px     | Caps the hit-time rope length (long fires get pulled in).   |
| `GRAPPLE_RETRACT_PXS`       | 800 px/s   | How fast the rope shortens while W is held.                 |
| `GRAPPLE_MIN_REST_LEN`      |  60 px     | Floor for rope length under retract; `0` would let the firer drift inside the anchor tile. |
| `GRAPPLE_INIT_MIN_LEN`      |  80 px     | Floor for the initial rope length (before any retract).     |
| `wpn->fire_rate_sec`        | 1.20 s     | Min interval between fires; gates chain re-fire too.        |

The hit-time rope length is set once when the head sticks (clamped pelvis-to-anchor distance). The W-hold retract is the only thing that changes it after that.

If the player wants to fire a new grapple while the current one is attached, they re-press `BTN_FIRE`: the fire path detects `state == GRAPPLE_ATTACHED`, calls `mech_grapple_release`, and falls through to spawn a fresh head — single press, full chain. The 1.20 s `fire_rate_sec` cooldown still gates how fast successive heads can launch.

## Solving CSTR_FIXED_ANCHOR

In `src/physics.c::solve_constraints_one_pass`. Note the early-return for the slack case — that's what makes the rope one-sided (the pendulum feel rather than a rigid stick).

```c
case CSTR_FIXED_ANCHOR: {
    int ai = c->a;
    if (p->inv_mass[ai] <= 0.0f) return;
    Vec2 fp = c->fixed_pos;
    float dx = fp.x - p->pos_x[ai];
    float dy = fp.y - p->pos_y[ai];
    float d2 = dx*dx + dy*dy;
    if (d2 < 1e-6f) return;
    float d = sqrtf(d2);
    if (d <= c->rest) return;       // rope slack — no force
    float diff = (d - c->rest) / d;
    p->pos_x[ai] += dx * diff;
    p->pos_y[ai] += dy * diff;
    break;
}
```

Inverse mass: the particle has its full inverse mass (no half-share), since the anchor end has effective inv_mass = 0.

## Release

```c
void grapple_release(World *w, int mech_id) {
    Mech *m = &w->mechs[mech_id];
    if (m->grapple.state == GRAPPLE_IDLE) return;
    if (m->grapple.constraint_idx >= 0) {
        w->constraints.items[m->grapple.constraint_idx].active = 0;
        m->grapple.constraint_idx = -1;
    }
    m->grapple.state = GRAPPLE_IDLE;
    audio_play_at(SFX_GRAPPLE_RELEASE, mech_pelvis_pos(w, mech_id));
}
```

We don't reclaim the constraint's slot in the pool — `active = 0` is enough; the existing constraint scanner skips inactive entries. Slot leakage is bounded (1 per grapple cycle, max ~40 per round per mech) and the pool is sized for 2048; never an issue.

## Anchor tracking when anchored to a moving mech

When the anchor is a bone particle on a target mech, the `CSTR_DISTANCE_LIMIT` constraint (with `min_len = 0`, `max_len = rope_length`) pulls *both* the firer and the target toward each other when the rope is stretched. Inside the rope length there's no force — the rope is slack. This means the firer can:

- Yank a heavier mech *toward* themselves when the firer suddenly accelerates away.
- Be tugged *with* a lighter mech that's running away from them.

The asymmetry comes from particle inv_mass — the chassis's mass scale. A Heavy chassis pulling a Scout pulls the Scout much more than the Scout pulls the Heavy.

If the target mech *dies* while attached, the constraint goes inactive (we release on death, see above).

If the target *limb* dismembers (e.g., I grappled their leg, they lost the leg), the limb's particles go dead — but the constraint stays attached to that detached particle, so I'm now grappled to a free-floating gibbed leg. That's hilarious; we keep it. (One could argue this is actually a feature: targeted dismemberment + grapple = "I tore off your leg and dragged it to me.")

## Cooldown

After every fire, `fire_cooldown = wpn->fire_rate_sec` (1.2 s). The grapple can't be re-fired during the cooldown. While `state == FLYING` a re-press is silently swallowed (no double-head). While `state == ATTACHED` a re-press auto-releases the current rope and falls through to fire a fresh head — but the cooldown still applies, so back-to-back chained fires are bounded to ≥1.2 s apart. (For tighter chains the right answer is to lower `fire_rate_sec` for the grapple specifically; that's a tuning decision.)

## Render

```c
// src/render.c::draw_mech — extend
if (m->grapple.state == GRAPPLE_FLYING) {
    // The flying head IS a projectile, drawn by projectile_draw.
    // We additionally draw a rope from the firer's hand to the head.
    Vec2 hand = mech_hand_pos(&w->world, mech_id);
    // Find the head's current position via projectile pool.
    int head_idx = projectile_find_grapple_head(&w->projectiles, mech_id);
    if (head_idx >= 0) {
        Vec2 head_pos = (Vec2){w->projectiles.pos_x[head_idx],
                               w->projectiles.pos_y[head_idx]};
        DrawLineEx(hand, head_pos, 1.5f, (Color){200, 200, 80, 220});
    }
}
if (m->grapple.state == GRAPPLE_ATTACHED) {
    Vec2 hand = mech_hand_pos(&w->world, mech_id);
    Vec2 anchor;
    if (m->grapple.anchor_mech == -1) {
        anchor = m->grapple.anchor_pos;
    } else {
        const Mech *t = &w->world.mechs[m->grapple.anchor_mech];
        int p = t->particle_base + m->grapple.anchor_part;
        anchor = (Vec2){w->world.particles.pos_x[p], w->world.particles.pos_y[p]};
    }
    DrawLineEx(hand, anchor, 1.5f, (Color){240, 220, 100, 255});
}
```

The rope is a single straight line. A flexing-rope shader (sampled bezier) is a nice-to-have for v1.5; not v1.

## Audio

Three SFX: fire, hit, release. The earlier "continuous rope tightening loop while contracting" cue is gone alongside the auto-contract design. See [09-audio.md](09-audio.md) §"SFX manifest".

## Edge cases worth being careful about

- **Grapple while in another grapple.** Re-press while ATTACHED chain-releases the current rope and fires a new head; re-press while FLYING is silently swallowed (no double-head).
- **Grapple anchor inside a SOLID tile.** Shouldn't happen (the projectile stops at the surface), but guard against floating-point error: clamp anchor_pos 4 px back along the flight direction before storing.
- **Two players grappled to each other simultaneously.** Each owns one rope. Both ropes are one-sided — they pull only when stretched. The two firers swing toward each other when both are flung out and stay slack between. Cinematic but bounded.
- **Grapple during ragdoll.** The dying player's grapple is released on transition to `!alive` (`mech_kill` calls `mech_grapple_release` first thing).
- **Grapple while reloading.** No interaction — grapple uses BTN_FIRE on the secondary slot, reload is per-slot.
- **Holding fire on the grapple.** WFIRE_GRAPPLE is edge-triggered. Holding fire doesn't re-fire.
- **Grapple while crouched.** Allowed; the rope's max-length limit acts on the pelvis regardless. Crouch lowers the mech but doesn't change the grapple math.

## Lag compensation

We do **not** lag-comp grapple hits. Like flag pickups (per the research in [00-overview.md](00-overview.md)), grapple anchor decisions use server-current positions. Reasoning: the firer is pulling themselves; the rubber-band correction on a missed grapple is sub-100ms and the player's local feedback is the rope head visibly hitting/missing.

Documented as a deliberate choice; not a trade-off worth a TRADE_OFFS entry.

## Map-design implications

The grapple unblocks specific level-design beats — places where the design assumes you can swing across a gap or zip up to a high catwalk. [07-maps.md](07-maps.md) flags which maps lean on grapple beats:

- **Catwalk** uses grapple-up-to-catwalk traversal in two spots.
- **Aurora** has two grapple-able overhangs above the central pit.
- **Slipstream** rewards grappling between catwalks (already vertical, but grapple makes it cheaper than jet).
- **Citadel** has grapple anchors near each flag base for fast retreat.

The other 4 maps (Foundry, Concourse, Reactor, Crossfire) are designed without dependence — they're playable without the grapple in your loadout, which is fine since the grapple is one of 6 secondary slot choices.

## Done when

- Grappling Hook secondary fires a head, the head sticks to tiles and bones, and the firer hangs at the hit-time rope length, swinging as a pendulum (one-sided rope, no auto-contract).
- Release on BTN_USE works; release on anchor-mech-death works; release on firer death works.
- Re-pressing BTN_FIRE while ATTACHED chain-releases + fires a new head (the user's "swing across, disconnect, fire and re-attach" Tarzan flow).
- The rope renders correctly for both local and remote players (grapple state on the snapshot's optional 8-byte suffix).
- The Catwalk map's grapple-up-to-catwalk beats are reachable; without grapple they're reachable only by chained jet jumps.
- The M3 trade-off "Grappling Hook is a stub" is deleted from TRADE_OFFS.md.
- Audio cues fire correctly (fire, hit, release).
- Shot tests `tests/shots/m5_grapple.shot` (basic) and `tests/shots/m5_grapple_swing.shot` (chain re-fire) verify the full cycle from log + screenshots.

## Trade-offs to log

- **Rope renders as a straight line.** No flex curve. Cosmetic; revisit if it looks bad.
- **No "missed grapple" rebound.** A grapple that hits nothing simply expires when its lifetime runs out (range / 1200 px/s = 0.5 s for a max-range miss). The head despawns silently.
- **Grapple anchor uses server-current position** (no lag comp). Documented above.
