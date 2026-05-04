# P11 — Per-weapon visible art + two-handed foregrip pose

## What this prompt does

Replaces the M4 generic-line-rifle render with a per-weapon sprite atlas. Each of the 14 weapons has its own visible silhouette held in the mech's R_HAND. Two-handed weapons (10 of 14) drive the L_HAND pose target to the foregrip position — partially resolves the M1 trade-off "left arm dangles."

Plus visible muzzle flash at the actual muzzle position (so bullets visibly emit from the correct point).

Depends on P10 (atlas runtime + alpha plumbing).

## Required reading

1. `CLAUDE.md`
2. `documents/04-combat.md` — weapon table (numbers don't change here; just visuals)
3. **`documents/m5/12-rigging-and-damage.md`** §"Per-weapon visible art", §"Two-handed weapons and the off-hand foregrip"
4. `documents/m5/11-art-direction.md` §"Pipeline 8 — Weapon atlas" — what's coming in P16
5. `TRADE_OFFS.md` — "Left hand has no pose target (no IK)"
6. `src/render.c` — current line-rifle drawing
7. `src/weapons.{c,h}` — `g_weapons[]` table; you'll add muzzle_offset references
8. `src/mech.c::build_pose` — pose target setup
9. `src/world.h` — `Weapon` struct (you may add visible-sprite metadata)

## Concrete tasks

### Task 1 — `src/weapon_sprites.{h,c}`

New module per `documents/m5/12-rigging-and-damage.md` §"Per-weapon visible art":

```c
typedef struct {
    Rectangle src;
    Vector2   pivot_grip;       // grip in src-relative px
    Vector2   pivot_foregrip;   // foregrip in src px; (-1,-1) for one-handed
    Vector2   muzzle_offset;    // muzzle in src-relative px
    float     draw_w, draw_h;
    int       weapon_id;
} WeaponSpriteDef;

extern Texture2D g_weapons_atlas;
extern const WeaponSpriteDef g_weapon_sprites[WEAPON_COUNT];

const WeaponSpriteDef *weapon_sprite_def(int weapon_id);
bool weapons_atlas_load(void);
void weapons_atlas_unload(void);
```

Hand-code the table per `documents/m5/12-rigging-and-damage.md` §"Per-weapon visible sizes" + §"Foregrip positions per two-handed weapon" — 14 entries. Use placeholder rectangles (50×50 etc.) until P16 provides the atlas. Pivot positions should match the spec values.

The WeaponSpriteDef table is the source of truth for muzzle position; existing `Weapon.muzzle_offset` (a single float) becomes a derived value.

### Task 2 — `draw_held_weapon` in `src/render.c`

Replace the line-rifle code (lines ~226-235) with:

```c
static void draw_held_weapon(const ParticlePool *p, const Mech *m, const Level *L, float alpha) {
    if (!m->alive) return;
    const WeaponSpriteDef *wp = weapon_sprite_def(m->weapon_id);
    if (!wp) return;
    int b = m->particle_base;
    Vec2 rh   = particle_render_pos(p, b + PART_R_HAND, alpha);
    Vec2 aim  = mech_aim_dir(/*world*/...);
    float angle = atan2f(aim.y, aim.x) * RAD2DEG;

    if (g_weapons_atlas.id != 0) {
        DrawTexturePro(g_weapons_atlas, wp->src,
                       (Rectangle){rh.x, rh.y, wp->draw_w, wp->draw_h},
                       wp->pivot_grip, angle,
                       m->is_dummy ? orange : WHITE);
    } else {
        /* Fallback: thin line for the barrel — same shape as M4 line-rifle, kept compatible. */
        Vec2 muzzle = { rh.x + aim.x * wp->draw_w * 0.7f, rh.y + aim.y * wp->draw_w * 0.7f };
        draw_bone_clamped(L, rh, muzzle, 3.0f, line_color);
    }
}
```

Plumb the call through draw_mech's z-order — weapon draws after the front arm, before the head (so the helmet's optic doesn't get covered) per `documents/m5/12-rigging-and-damage.md` §"Render order".

### Task 3 — Two-handed foregrip pose

In `mech.c::build_pose`, after right-arm aim drive:

```c
const WeaponSpriteDef *wp = weapon_sprite_def(m->weapon_id);
if (wp && wp->pivot_foregrip.x >= 0.0f) {
    Vec2 rh  = particle_pos(p, b + PART_R_HAND);
    Vec2 aim = mech_aim_dir(world, mech_id);
    float perp_x = -aim.y, perp_y = aim.x;
    float along = wp->pivot_foregrip.x - wp->pivot_grip.x;
    float perp  = wp->pivot_foregrip.y - wp->pivot_grip.y;
    Vec2 foregrip = {
        rh.x + aim.x * along + perp_x * perp,
        rh.y + aim.y * along + perp_y * perp,
    };
    pose->target_pos[PART_L_HAND] = foregrip;
    pose->strength[PART_L_HAND]   = 0.6f;
}
```

When the secondary slot is held (one-handed weapon), `pivot_foregrip = (-1, -1)` and the L_HAND pose stays unset — left arm dangles per the M1 default.

### Task 4 — Visible muzzle position drives bullet origin

The fire path (`weapons.c::weapons_fire_*`) computes muzzle as `hand + dir * muzzle_offset`. Update this to read `muzzle_offset` from the weapon sprite def:

```c
Vec2 muzzle_world(Vec2 rh, Vec2 aim, const WeaponSpriteDef *wp) {
    float along = wp->muzzle_offset.x - wp->pivot_grip.x;
    float perp  = wp->muzzle_offset.y - wp->pivot_grip.y;
    return (Vec2){
        rh.x + aim.x * along + (-aim.y) * perp,
        rh.y + aim.y * along + (aim.x)  * perp,
    };
}
```

Replace `Weapon.muzzle_offset` (single float) call sites in weapons.c with `muzzle_world(rh, aim, weapon_sprite_def(...))`. The visible muzzle and the physics muzzle now coincide.

### Task 5 — Muzzle flash at the muzzle

Per `documents/m5/12-rigging-and-damage.md` §"Render path":

When the weapon was just fired (`m->fire_cooldown > wpn->fire_rate_sec - 0.05f`, i.e. fired in the last 50 ms), draw a small additive muzzle-flash sprite at the muzzle world position.

Use the existing FX pool — spawn an `FX_TRACER`-style sprite with very short life and additive blend. The flash fades over a few ticks.

For now, draw as a simple 8-px additive yellow-orange circle at muzzle position. Final flash sprites are P13's HUD/atlas pass.

### Task 6 — RMB secondary fire shows the right weapon

When `BTN_FIRE_SECONDARY` (RMB, P09) fires, the renderer should briefly show the secondary weapon at R_HAND for one tick, then return to the primary. Implementation: when the secondary just fired (tick the secondary's cooldown / 50 ms ago), `draw_held_weapon` swaps to the secondary's sprite for that frame.

Implementation: track `m->last_fired_slot` (0 or 1) and `m->last_fired_tick`. If `(w->tick - m->last_fired_tick) < 3` and `last_fired_slot != active_slot`, draw the OTHER slot's weapon. Otherwise draw the active slot.

## Done when

- `make` builds clean.
- A Trooper holding Pulse Rifle: rifle silhouette is visible at R_HAND, oriented along aim.
- Switching to Sidearm (Q): visible weapon shrinks to small pistol; left hand drops from foregrip.
- Switching back: left hand returns to foregrip.
- Mass Driver visibly takes up most of the mech's height; Sidearm is barely visible.
- Bullets emit from the visible muzzle (verify by firing toward a wall and checking the spark position vs. the rendered muzzle).
- RMB-fires-secondary: visible weapon flickers to grenade for one frame, then back to rifle.
- Capsule + line-rifle fallback works when no atlas is loaded.

## Out of scope

- Real weapon atlas art: P16 generates it.
- Hit-flash on the weapon: not a thing; just on the body (P12).
- Per-weapon-class muzzle-flash sprite art: P13 generates it.

## How to verify

```bash
make
./soldut --shot tests/shots/m5_weapon_held.shot
```

Write `tests/shots/m5_weapon_held.shot` that cycles through chassis × weapon combinations and captures a contact sheet.

## Close-out

1. Update `CURRENT_STATE.md`: per-weapon visible art runtime in.
2. Update `TRADE_OFFS.md`:
   - **Update** "Left hand has no pose target (no IK)" — note that two-handed weapons now drive L_HAND via foregrip; one-handed still dangles.
3. Don't commit unless explicitly asked.

## Common pitfalls

- **`pivot_grip` and `pivot_foregrip` in src-relative px**: same coordinate space as the sub-rect. Don't accidentally use atlas-global px.
- **Aim direction vs bone direction**: the weapon rotates by aim angle, NOT by the forearm bone angle. R_HAND is the anchor; aim is the orientation.
- **Reload state**: if `m->reload_timer > 0`, you might want to draw the weapon at a slightly different angle (lowered, magazine partially out). For v1, just draw it normally — animation polish is M6.
- **Foregrip on two-handed weapons during reload**: the M1 trade-off note. Pose strength 0.6 + the constraint solver settles into a sensible position even when the bone chains are stressed by reload pose.
- **Last-fired-slot tracking**: don't accidentally show the secondary forever after one RMB; clamp to a few ticks.
- **muzzle_world vs old `Weapon.muzzle_offset`**: keep the old field for backwards compat in case any code path still reads it; have the muzzle helper prefer the sprite-def version.
