# P06 — Grappling hook implementation

## What this prompt does

Implements the Grappling Hook secondary weapon. Currently a stub: `g_weapons[WEAPON_GRAPPLING_HOOK]` is registered with `WFIRE_GRAPPLE` but `mech_try_fire`'s WFIRE_GRAPPLE case only logs `"NOT YET IMPLEMENTED"`. After this prompt: a hook head launches, sticks to tiles or bones, contracting distance constraint pulls the firer toward the anchor, BTN_USE releases.

Depends on P01 (`.lvl` format), P02 (polygon collision — for tile anchor on slopes/polygons).

## Required reading

1. `CLAUDE.md`
2. `documents/04-combat.md` — weapon design philosophy, the secondaries table
3. **`documents/m5/05-grapple.md`** — the spec for this prompt
4. `documents/m5/12-rigging-and-damage.md` §"Per-weapon visible art" — for the eventual visible grapple sprite (out of scope here; just leave the visual hook as a single line for now)
5. `TRADE_OFFS.md` — "Grappling Hook is a stub"
6. `src/weapons.{c,h}` — `g_weapons[WEAPON_GRAPPLING_HOOK]`, `WFIRE_GRAPPLE` enum
7. `src/projectile.{c,h}` — pool, `ProjectileKind` enum (you'll add `PROJ_GRAPPLE_HEAD`)
8. `src/mech.{c,h}` — `mech_try_fire`, `mech_step_drive`
9. `src/world.h` — `ConstraintKind` enum (you'll add `CSTR_FIXED_ANCHOR`); `Mech` struct
10. `src/physics.c` — `solve_distance` and friends (you'll add `solve_fixed_anchor`)

## Background

The Grappling Hook is a utility secondary that pulls the firer toward an anchor. Soldat / Worms / Spider-Man tradition. Pure utility — zero damage. Map design beats in P17/P18 (Catwalk, Aurora, Crossfire, Citadel) lean on it.

## Concrete tasks

### Task 1 — Grapple state on Mech

Per `documents/m5/05-grapple.md` §"Data model":

```c
// src/world.h — added
typedef enum { GRAPPLE_IDLE = 0, GRAPPLE_FLYING, GRAPPLE_ATTACHED } GrappleState;

typedef struct {
    GrappleState state;
    Vec2         anchor_pos;
    int          anchor_mech;            // -1 if tile-anchored
    int          anchor_part;            // PART_* if anchor_mech >= 0
    float        rest_length;            // contracts each tick
    int          constraint_idx;         // index in world.constraints; -1 if none
} Grapple;

// World struct adds:
Grapple grapples[MAX_MECHS];
```

### Task 2 — `PROJ_GRAPPLE_HEAD`

In `src/world.h::ProjectileKind`, add `PROJ_GRAPPLE_HEAD`. Speed 1200 px/s, gravity_scale 0, range 600 px.

In `projectile.c::projectile_step`, special-case `PROJ_GRAPPLE_HEAD` per `documents/m5/05-grapple.md` §"The hook head":
- On tile hit: stick. Set firer's `grapple.state = GRAPPLE_ATTACHED`, anchor_pos, anchor_mech=-1. Attach a `CSTR_FIXED_ANCHOR` constraint. Despawn head.
- On bone hit: stick to that bone particle. anchor_mech = mech_id, anchor_part = bone_part. Use a regular `CSTR_DISTANCE` constraint. The grappled mech takes no damage.
- On lifetime expiry: reset state to IDLE, despawn.

### Task 3 — `CSTR_FIXED_ANCHOR` constraint kind

In `src/world.h::ConstraintKind`, add `CSTR_FIXED_ANCHOR`. Add `Vec2 fixed_pos;` to the `Constraint` struct (per `documents/m5/05-grapple.md` §"Pull mechanism: contracting distance constraint").

In `src/physics.c`, add `solve_fixed_anchor`:

```c
case CSTR_FIXED_ANCHOR: {
    int ai = c->a;
    Vec2 fp = c->fixed_pos;
    float dx = fp.x - p->pos_x[ai];
    float dy = fp.y - p->pos_y[ai];
    float d2 = dx*dx + dy*dy;
    if (d2 < 1e-6f) break;
    float d = sqrtf(d2);
    float diff = (d - c->rest) / d;
    p->pos_x[ai] += dx * diff;       // particle has full inv_mass; anchor effectively 0
    p->pos_y[ai] += dy * diff;
    break;
}
```

### Task 4 — Per-tick contraction + release

In `mech_step_drive`, when `m->grapple.state == GRAPPLE_ATTACHED`:

```c
float contract_rate = 800.0f * dt;
m->grapple.rest_length -= contract_rate;
if (m->grapple.rest_length < 80.0f) m->grapple.rest_length = 80.0f;
Constraint *c = &w->constraints.items[m->grapple.constraint_idx];
c->rest = m->grapple.rest_length;
if (m->grapple.anchor_mech == -1) c->fixed_pos = m->grapple.anchor_pos;

// Release on BTN_USE edge
if ((pressed & BTN_USE)) grapple_release(w, mid);
// Release if anchor mech dies
if (m->grapple.anchor_mech >= 0 && !w->mechs[m->grapple.anchor_mech].alive)
    grapple_release(w, mid);
```

`grapple_release` deactivates the constraint, resets state to IDLE.

### Task 5 — Fire path

Replace the "NOT YET IMPLEMENTED" log in `mech_try_fire`'s WFIRE_GRAPPLE case with: spawn a `PROJ_GRAPPLE_HEAD` projectile from the mech's right hand toward the aim direction. Set `m->grapple.state = GRAPPLE_FLYING`. Cooldown applies (1.2 s).

If `m->grapple.state != IDLE` when fire is pressed, ignore the press. Holding fire doesn't re-fire.

### Task 6 — Snapshot grapple state

Per `documents/m5/05-grapple.md` §"Wire format":

Add `SNAP_DIRTY_GRAPPLE = 1u << 11` to the per-entity dirty mask. When grapple state is non-IDLE, ship 4 bytes per dirty mech with packed `(state, anchor_x, anchor_y, anchor_mech)`. When IDLE, omit (the 99% case keeps idle bandwidth flat).

### Task 7 — Render

Per `documents/m5/05-grapple.md` §"Render":

In `src/render.c::draw_mech`, after drawing the front arm:

```c
if (m->grapple.state == GRAPPLE_FLYING) {
    Vec2 hand = mech_hand_pos(w, mech_id);
    int head_idx = projectile_find_grapple_head(&w->projectiles, mech_id);
    if (head_idx >= 0) {
        Vec2 head_pos = (Vec2){w->projectiles.pos_x[head_idx], w->projectiles.pos_y[head_idx]};
        DrawLineEx(hand, head_pos, 1.5f, (Color){200, 200, 80, 220});
    }
}
if (m->grapple.state == GRAPPLE_ATTACHED) {
    Vec2 hand = mech_hand_pos(w, mech_id);
    Vec2 anchor = (m->grapple.anchor_mech == -1) ? m->grapple.anchor_pos
                : particle_pos(&w->particles, ... bone particle ...);
    DrawLineEx(hand, anchor, 1.5f, (Color){240, 220, 100, 255});
}
```

Single straight line for the rope. Flexing-rope shader is M6 polish.

## Done when

- `make` builds clean.
- A mech with Grappling Hook secondary, switched to active (Q to swap), pressing fire: head flies toward cursor; on tile hit, mech is pulled toward the anchor; pressing BTN_USE releases.
- Grappling a moving mech: the moving mech also gets pulled toward the firer (proportional to mass via constraint solver).
- A grappled mech that dies releases the firer.
- Cooldown 1.2 s applies between grapple uses.
- A second BTN_FIRE press while state != IDLE is silently ignored.
- Wire format: idle grapple costs 0 bytes/snapshot; active grapple ~4 bytes per snapshot.
- Network test still passes; `tests/shots/m5_grapple.shot` shows the full cycle.

## Out of scope

- Visible weapon-in-hand grappling-hook sprite — that's P11 (per-weapon art).
- Audio cues for grapple_fire / grapple_hit / grapple_release / grapple_pull_loop — that's P14 (audio module). Wire the call sites with no-op stubs.
- Lag compensation on grapple anchor decisions — explicitly NOT done (server-current). Documented.

## How to verify

```bash
make
./tests/net/run.sh
```

Write `tests/shots/m5_grapple.shot`:

```
seed 1 0
window 1280 720
spawn_at 1600 800
# Make sure secondary is grapple
at 10 tap swap     # active = secondary
at 30 mouse 1900 700  # cursor toward upper-right
at 40 tap fire     # launches grapple
at 80 shot grapple_flight
at 200 shot grapple_pull
at 400 tap use     # release
at 450 shot grapple_release
at 500 end
```

## Close-out

1. Update `CURRENT_STATE.md`: grappling hook full implementation.
2. Update `TRADE_OFFS.md`: **delete** "Grappling Hook is a stub". **Add** "Grapple anchor uses server-current position (no lag comp)" and "Rope renders as a straight line" (pre-disclosed).
3. Don't commit unless explicitly asked.

## Common pitfalls

- **`CSTR_FIXED_ANCHOR` requires the particle to have non-zero inv_mass** — pelvis/hand should already; verify before solving.
- **The grapple constraint slot in the pool isn't reclaimed on release** — `active = 0` is enough; the existing constraint scanner skips inactive entries. Slot leakage is bounded.
- **Edge-detection for BTN_USE release**: don't release on held BTN_USE; only on a fresh press. Use `pressed & BTN_USE`.
- **Grapple anchor inside a SOLID tile due to FP error** — clamp anchor_pos out of any solid tile by 4 px before storing.
- **Holding BTN_FIRE re-fires** — WFIRE_GRAPPLE should be edge-triggered. Gate on `pressed`, not `buttons`.
- **Grapple while reloading**: the reload state is per-slot; grapple is a secondary. Active slot = secondary, no reload conflict expected.
