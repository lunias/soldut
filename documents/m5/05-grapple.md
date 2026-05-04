# M5 — Grappling hook

The Grappling Hook secondary weapon. Currently a stub: `src/weapons.c::g_weapons[WEAPON_GRAPPLING_HOOK]` is registered with `fire = WFIRE_GRAPPLE` and `range_px = 600`, but `mech_try_fire`'s WFIRE_GRAPPLE case only logs `"grapple_attempt (NOT YET IMPLEMENTED)"` and ticks the cooldown.

This doc lands the proper implementation. Roughly one engineer-day of work; pairs with map-design beats that need swing/pull movement (per [07-maps.md](07-maps.md)).

## What the player feels

Press fire (with Grappling Hook as the active secondary):

1. A small projectile launches from the right hand toward the cursor.
2. On hit (tile or mech bone), the rope goes taut. The player is **pulled** toward the anchor at a contracting rate.
3. Pull continues until the player presses BTN_USE (release), or the contraction reaches the minimum length, or the anchor's mech dies, or the player dies.
4. While pulled, the player can still aim and shoot — the grapple doesn't lock you out of combat.

This is the Soldat / Worms / Spider-Man tradition. It's pure utility — zero damage — but the right map design makes it transformative for vertical traversal.

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

A grapple's state needs to ride snapshots so remote clients can render the rope correctly. Cheap encoding: 4 bytes per mech.

```c
// EntitySnapshot extension — not adding a separate field, just packing
// the 3-bit state + anchor position into existing reserved space.
//
// We use 1 of the 5 spare bits in the new state_bits u16, plus 2 new
// reserved bytes... actually, dedicated:
//
//   grapple_state    : 2 bits  (GRAPPLE_IDLE/FLYING/ATTACHED, 3 values)
//   grapple_anchor_x : 13 bits (signed, ±4096 px in 1-px res)
//   grapple_anchor_y : 13 bits
//   grapple_anchor_mech : 5 bits (-1 = no mech anchor; 0..30 = mech_id)
//
// Pack into 32 bits = 4 bytes.
```

Bumping the EntitySnapshot from 28 → 32 bytes pushes the bandwidth budget but stays within the 80 kbps target: 32 × 32 mechs × 30 Hz = 30 KB/s downstream = 240 kbps. **That exceeds the 80 kbps target.**

Mitigation: only ship the grapple bytes when the grapple is non-IDLE. Add a `SNAP_DIRTY_GRAPPLE` bit to the per-entity dirty mask; when IDLE (the 99% case) the bytes are absent.

```c
// snapshot.h — added
enum {
    SNAP_DIRTY_GRAPPLE = 1u << 11,
};
```

This keeps idle bandwidth flat. Active grapples cost ~4 bytes × however-many-mechs-are-grappling × 30 Hz ≈ 480 B/s per active grapple. With 4 simultaneous active grapples (very unusual): 2 KB/s. Trivial.

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

## Pull mechanism: contracting distance constraint

When attached, we install a distance constraint between the firer's pelvis particle and an "anchor particle." The constraint solver does the rest — every relaxation iteration pulls the pelvis toward the anchor by the constraint's normal physics.

Two shapes:

- **Tile anchor** (`anchor_mech == -1`): the anchor is a virtual particle, fixed at `anchor_pos`. We don't actually allocate it in the particle pool — instead, the constraint solver has a special case for "one end of this constraint is a fixed Vec2." Cleanest implementation: add a `CSTR_FIXED_ANCHOR` constraint kind that holds the anchor position inline.
- **Mech anchor** (`anchor_mech >= 0`): the anchor is the bone particle of the target mech. Use the existing `CSTR_DISTANCE` shape with `a = grappler_pelvis, b = target_bone_particle`.

```c
// src/world.h — new constraint kind
typedef enum {
    CSTR_DISTANCE = 0,
    CSTR_DISTANCE_LIMIT,
    CSTR_ANGLE,
    CSTR_FIXED_ANCHOR,        // new: end b is a fixed Vec2 stored inline
} ConstraintKind;

typedef struct {
    uint16_t a, b;
    uint16_t c;
    uint8_t  kind;
    uint8_t  active;
    float    rest;
    float    min_len, max_len;
    float    min_ang, max_ang;
    Vec2     fixed_pos;       // new: used for CSTR_FIXED_ANCHOR
} Constraint;
```

Each tick of the simulation (in `mech_step_drive`), we tick the rest length down:

```c
// src/mech.c::mech_step_drive — when grapple is ATTACHED
if (m->grapple.state == GRAPPLE_ATTACHED) {
    float contract_rate = 800.0f * dt;   // 800 px/s
    m->grapple.rest_length -= contract_rate;
    if (m->grapple.rest_length < 80.0f) m->grapple.rest_length = 80.0f;
    Constraint *c = &w->constraints.items[m->grapple.constraint_idx];
    c->rest = m->grapple.rest_length;
    if (m->grapple.anchor_mech == -1) {
        c->fixed_pos = m->grapple.anchor_pos;
    }
    // (For mech anchor, the anchor particle moves with the target — no
    //  per-tick update needed.)
    // Release on BTN_USE edge.
    if ((m->latched_input.buttons & BTN_USE) &&
        !(m->prev_buttons & BTN_USE))
    {
        grapple_release(w, mid);
    }
    // Release if anchor mech dies.
    if (m->grapple.anchor_mech >= 0 &&
        !w->mechs[m->grapple.anchor_mech].alive)
    {
        grapple_release(w, mid);
    }
}
```

The 800 px/s contract rate is starting; tune in playtest. Min length 80 px keeps the firer from face-planting into the anchor.

## Solving CSTR_FIXED_ANCHOR

Add to `src/physics.c::physics_constrain_and_collide`:

```c
case CSTR_FIXED_ANCHOR: {
    int ai = c->a;
    Vec2 fp = c->fixed_pos;
    float dx = fp.x - w->particles.pos_x[ai];
    float dy = fp.y - w->particles.pos_y[ai];
    float d2 = dx*dx + dy*dy;
    if (d2 < 1e-6f) break;
    float d = sqrtf(d2);
    float diff = (d - c->rest) / d;
    // Move only the particle (the anchor is fixed).
    w->particles.pos_x[ai] += dx * diff;
    w->particles.pos_y[ai] += dy * diff;
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

When the anchor is a bone particle on a target mech, the constraint pulls *both* the firer and the target — symmetric distance constraint. This means the firer can:

- Pull a heavier mech *toward* themselves.
- Be pulled *with* a lighter mech that's running away.

The asymmetry comes from particle inv_mass — the chassis's mass scale. A Heavy chassis pulling a Scout will pull the Scout much more than the Scout pulls the Heavy.

If the target mech *dies* while attached, the constraint goes inactive (we release on death, see above).

If the target *limb* dismembers (e.g., I grappled their leg, they lost the leg), the limb's particles go dead — but the constraint stays attached to that detached particle, so I'm now grappled to a free-floating gibbed leg. That's hilarious; we keep it. (One could argue this is actually a feature: targeted dismemberment + grapple = "I tore off your leg and dragged it to me.")

## Cooldown

After release, `fire_cooldown = wpn->fire_rate_sec` (1.2 s). The grapple can't be re-fired during this. While `state == FLYING`, a second BTN_FIRE press is silently ignored.

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

Three SFX: fire, hit, release. Plus a continuous "rope tightening" loop while contracting. See [09-audio.md](09-audio.md) §"SFX manifest".

## Edge cases worth being careful about

- **Grapple while in another grapple.** Second BTN_FIRE press ignored unless state == IDLE.
- **Grapple anchor inside a SOLID tile.** Shouldn't happen (the projectile stops at the surface), but guard against floating-point error: clamp anchor_pos out of any solid tile by 4 px before storing.
- **Two players grappled to each other simultaneously.** Each owns one constraint; both contract; they meet in the middle. Cinematic.
- **Grapple during ragdoll.** The dying player's grapple is released on transition to `!alive`.
- **Grapple while reloading.** No interaction — grapple uses BTN_FIRE on the secondary slot, reload is per-slot, the active slot is the secondary.
- **Holding fire on the grapple.** WFIRE_GRAPPLE is edge-triggered (we already gate on `prev_buttons & BTN_FIRE` in the fire path). Holding fire doesn't re-fire.
- **Grapple while crouched.** Allowed; the contracting constraint pulls the pelvis. Crouch lowers the mech but doesn't change the grapple math.

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

- Grappling Hook secondary fires a head, the head sticks to tiles and bones, and the firer is pulled toward the anchor.
- Release on BTN_USE works; release on anchor-mech-death works.
- The rope renders correctly for both local and remote players (grapple state in snapshot delta).
- The Catwalk map's grapple-up-to-catwalk beats are reachable; without grapple they're reachable only by chained jet jumps.
- The M3 trade-off "Grappling Hook is a stub" is deleted from TRADE_OFFS.md.
- Audio cues fire correctly (fire, hit, release, contract loop).
- A shot test (`tests/shots/m5_grapple.shot`) verifies a single grapple-and-pull cycle.

## Trade-offs to log

- **Rope renders as a straight line.** No flex curve. Cosmetic; revisit if it looks bad.
- **No "missed grapple" rebound.** A grapple that hits nothing simply expires when its lifetime runs out (range / 1200 px/s = 0.5 s for a max-range miss). The head despawns silently.
- **Grapple anchor uses server-current position** (no lag comp). Documented above.
