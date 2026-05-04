# P10 — Mech sprite atlas runtime + per-chassis bone-length distinctness + posture

## What this prompt does

Replaces the M4 capsule-line-bone rendering with sprite-atlas-per-chassis. Each chassis loads its own atlas with the per-part sub-rects + pivot data. The capsule fallback stays for fresh-checkout/no-asset cases. Also lands the per-chassis bone-length distinctness pass — Heavy is visibly bigger, Scout smaller, Sniper hunched — and the per-chassis posture quirks.

Depends on P03 (interpolation alpha plumbed through draw_mech). Asset content lands in P15/P16.

## Required reading

1. `CLAUDE.md`
2. `documents/06-rendering-audio.md` §"Drawing the mechs"
3. **`documents/m5/12-rigging-and-damage.md`** — the spec, especially §"Per-chassis bone structures and silhouettes" + §"Sprite anatomy: rigid skinning + overlap zones"
4. `documents/m5/08-rendering.md` §"Mech sprite atlases"
5. `src/world.h` — ParticlePool, Mech
6. `src/render.c` — `draw_mech` (the M4 capsule version)
7. `src/mech.c` — `g_chassis[]` table (lines 24–78 per the previous Bash grep)
8. `src/mech.c::build_pose` — pose target setup

## Concrete tasks

### Task 1 — Aggressive per-chassis bone-length distinctness

In `src/mech.c::g_chassis[]`, replace the M4 values per `documents/m5/12-rigging-and-damage.md` §"Per-chassis bone structures":

```c
[CHASSIS_TROOPER] = { ... .bone_arm = 14, .bone_forearm = 16, .bone_thigh = 18, .bone_shin = 18, .torso_h = 30, .neck_h = 14, .hitbox_scale = 1.0f },
[CHASSIS_SCOUT]   = { ... .bone_arm = 11, .bone_forearm = 13, .bone_thigh = 14, .bone_shin = 14, .torso_h = 24, .neck_h = 12, .hitbox_scale = 0.85f },
[CHASSIS_HEAVY]   = { ... .bone_arm = 17, .bone_forearm = 18, .bone_thigh = 20, .bone_shin = 20, .torso_h = 38, .neck_h = 16, .hitbox_scale = 1.2f },
[CHASSIS_SNIPER]  = { ... .bone_arm = 13, .bone_forearm = 19, .bone_thigh = 17, .bone_shin = 21, .torso_h = 30, .neck_h = 16, .hitbox_scale = 1.0f },
[CHASSIS_ENGINEER]= { ... .bone_arm = 14, .bone_forearm = 14, .bone_thigh = 16, .bone_shin = 18, .torso_h = 32, .neck_h = 13, .hitbox_scale = 0.95f },
```

Verify the constraint rest lengths come from these fields (mech_create_loadout / `add_distance` calls). The existing M4 code already reads these — just changing the values is enough.

### Task 2 — Per-chassis posture quirks in `build_pose`

Per `documents/m5/12-rigging-and-damage.md` §"Posture differences per chassis":

In `src/mech.c::build_pose`:

```c
switch (m->chassis_id) {
    case CHASSIS_SCOUT:
        // forward lean: chest pose target offset by +2 px x toward facing.
        chest_target.x += (m->facing_left ? -2.0f : 2.0f);
        break;
    case CHASSIS_HEAVY:
        // locked upright: bump chest pose strength.
        pose->strength[PART_CHEST] = 0.85f;   // up from 0.7
        break;
    case CHASSIS_SNIPER:
        // hunched: head target offset down + forward.
        head_target.x += (m->facing_left ? -2.0f : 2.0f);
        head_target.y += 3.0f;
        break;
    case CHASSIS_ENGINEER:
        // skip right-arm aim drive when active slot is secondary (engineer holds tools, not rifles)
        if (m->active_slot == 1) skip_right_arm_aim = true;
        break;
    default: break;
}
```

### Task 3 — `src/mech_sprites.{h,c}`

Per `documents/m5/12-rigging-and-damage.md` §"Pivot specification":

```c
typedef struct {
    Rectangle src;
    Vector2   pivot;
    float     draw_w, draw_h;
} MechSpritePart;

typedef enum {
    MSP_TORSO=0, MSP_HEAD, MSP_HIP_PLATE,
    MSP_SHOULDER_L, MSP_SHOULDER_R,
    MSP_ARM_UPPER_L, MSP_ARM_UPPER_R,
    MSP_ARM_LOWER_L, MSP_ARM_LOWER_R,
    MSP_HAND_L, MSP_HAND_R,
    MSP_LEG_UPPER_L, MSP_LEG_UPPER_R,
    MSP_LEG_LOWER_L, MSP_LEG_LOWER_R,
    MSP_FOOT_L, MSP_FOOT_R,
    MSP_STUMP_SHOULDER_L, MSP_STUMP_SHOULDER_R,
    MSP_STUMP_HIP_L, MSP_STUMP_HIP_R,
    MSP_STUMP_NECK,
    MSP_COUNT
} MechSpriteId;

typedef struct {
    Texture2D atlas;
    MechSpritePart parts[MSP_COUNT];
} MechSpriteSet;

extern MechSpriteSet g_chassis_sprites[CHASSIS_COUNT];

bool mech_sprites_load_all(void);    // walks `assets/sprites/<chassis>.png`
void mech_sprites_unload_all(void);
```

Per-chassis hand-coded `g_chassis_sprites[chassis_id].parts[]` table — placeholder values until P15 produces real atlas art. Use plausible sub-rects (e.g., torso at (0,0,56,72), head at (60,0,40,40), etc.) so the table compiles and the runtime can use them once art arrives.

### Task 4 — Replace `draw_mech` with sprite version

Per `documents/m5/08-rendering.md` §"Replacing draw_mech":

```c
static void draw_mech(const ParticlePool *p, const ConstraintPool *cp,
                      const Mech *m, const Level *L, float alpha) {
    const MechSpriteSet *set = &g_chassis_sprites[m->chassis_id];
    if (set->atlas.id == 0) {
        draw_mech_capsules(p, cp, m, L, alpha);   // M4 fallback, kept
        return;
    }
    /* Walk render parts in z-order (back leg, back arm, body, front, head, weapon). */
    for (int i = 0; i < MECH_RENDER_PART_COUNT; ++i) {
        const MechRenderPart *rp = &g_render_parts[i];
        if (!bone_active_or_no_constraint(cp, m->particle_base + rp->part_a, m->particle_base + rp->part_b)) continue;
        Vec2 a = particle_render_pos(p, m->particle_base + rp->part_a, alpha);
        Vec2 b = particle_render_pos(p, m->particle_base + rp->part_b, alpha);
        float angle = atan2f(b.y - a.y, b.x - a.x) * RAD2DEG;
        Vec2 mid = (Vec2){(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const MechSpritePart *sp = &set->parts[rp->sprite_idx];
        DrawTexturePro(set->atlas, sp->src,
                       (Rectangle){mid.x, mid.y, sp->draw_w, sp->draw_h},
                       sp->pivot, angle,
                       m->is_dummy ? orange_tint : (m->alive ? WHITE : red_tint));
    }
    /* Stump caps land in P12. */
}
```

Keep `draw_mech_capsules` as the M4 fallback verbatim; just rename. The condition `set->atlas.id == 0` flips to sprite mode once P15 has loaded the atlas.

### Task 5 — Render order table

Per `documents/m5/12-rigging-and-damage.md` §"Render order":

```c
static const MechRenderPart g_render_parts[] = {
    /* Back leg */
    { .part_a = PART_L_HIP,  .part_b = PART_L_KNEE,  .sprite_idx = MSP_LEG_UPPER_L },
    /* ... */
    /* Torso */
    { .part_a = PART_CHEST,  .part_b = PART_PELVIS,  .sprite_idx = MSP_TORSO },
    { .part_a = -1,          .part_b = PART_PELVIS,  .sprite_idx = MSP_HIP_PLATE },   // anchor at single particle
    /* ... etc */
};
#define MECH_RENDER_PART_COUNT 17
```

The hip plate / shoulder plate are drawn at a single particle (no bone). Use a `part_a == -1` convention to mean "anchor at part_b only".

### Task 6 — Atlas load path

In `src/main.c::game_init` or after platform init: call `mech_sprites_load_all()`. The function tries `assets/sprites/<chassis>.png` for each chassis; if missing, sets `set->atlas.id = 0` and the renderer falls back to capsules per chassis.

The capsule fallback works for some chassis and atlas for others if mid-development (only Trooper has art ready, etc.).

## Done when

- `make` builds clean.
- With no atlases: render is identical to M4 capsule mech.
- With a placeholder atlas dropped at `assets/sprites/trooper.png`: Trooper renders as the atlas; other chassis fall back to capsule.
- Per-chassis bone lengths are visibly different — Scout standing next to a Heavy: Heavy is ~30% taller and wider.
- Sniper's pose has a slight forward head lean.
- Heavy's chest holds rigidly upright under sustained jet.
- Engineer holding the secondary slot (a tool-equivalent for now): right arm doesn't aim-drive at the cursor.

## Out of scope

- The actual atlas art — P15/P16.
- Stump caps + dismemberment visuals — P12.
- Damage feedback (hit-flash, decals, smoke) — P12.
- Held-weapon sprites — P11.

## How to verify

```bash
make
./soldut --host 23073
# Spawn each chassis, observe relative sizes
```

Write a shot test `tests/shots/m5_chassis_distinctness.shot` that spawns each chassis side-by-side and captures.

## Close-out

1. Update `CURRENT_STATE.md`: per-chassis distinctness + atlas runtime.
2. Update `TRADE_OFFS.md` — no entries to delete yet (the capsule-rendering trade-off resolves in P12 once damage feedback is also in).
3. Don't commit unless explicitly asked.

## Common pitfalls

- **Bone-length changes ripple through `mech_step_drive`'s pose target math** — check the existing pose targets compute from chassis fields. The M4 code already does; if not, add it.
- **`hitbox_scale` is currently unused** — it's a sprite/visual scale knob. M5 uses it via `draw_w/draw_h` proportionally.
- **Atlas not found**: don't fail loudly. Log a single warning, fall back to capsule. Lets the build run on a checkout without assets.
- **Render-order table size**: 17 entries. Off-by-one will skip a part or read out-of-bounds. Add a `static_assert(sizeof(g_render_parts)/sizeof(g_render_parts[0]) == MECH_RENDER_PART_COUNT)`.
- **`particle_render_pos` requires `alpha`**: plumb through. P03 set this up; if it didn't, you can fall back to `particle_pos` until P03 lands.
