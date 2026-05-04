# P12 — Damage feedback (hit-flash + decals + stump caps + smoke)

## What this prompt does

Lands the four runtime damage-feedback layers on top of the static sprite atlas:

1. **Hit-flash tint** — white-additive flash for ~80–120 ms after damage.
2. **Persistent damage decals** — per-limb ring of small dent/scorch sprites composited over each part.
3. **Stump caps** — when a limb dismembers, a sprite covers the joint where the limb was.
4. **Smoke from heavily damaged limbs** — particle emitter triggered when a limb's HP drops below 30% of max.

Plus the heavy initial blood spray + pinned stump emitter on dismemberment.

Depends on P10 (atlas runtime). Resolves the M3 trade-off "Mechs rendered as raw capsules" once everything's in.

## Required reading

1. `CLAUDE.md`
2. `documents/04-combat.md` §"Blood", §"Decals", §"Gibs"
3. **`documents/m5/12-rigging-and-damage.md`** §"Damage feedback — three layers", §"Dismemberment visuals"
4. `src/mech.{c,h}` — `mech_apply_damage`, `mech_kill`, `mech_dismember`
5. `src/world.h` — Mech, FxPool
6. `src/particle.{c,h}` — fx_spawn_blood, fx_spawn_spark; you'll add `fx_spawn_smoke` and `fx_spawn_stump_emitter`
7. `src/render.c` — draw_mech (extended in P10)
8. `src/decal.{c,h}` — splat layer

## Concrete tasks

### Task 1 — Hit-flash tint

Per `documents/m5/12-rigging-and-damage.md` §"Layer 1 — Hit-flash tint":

Repurpose `Mech.last_damage_taken` (already present, currently underused) as a 0–0.10 s timer.

In `mech_apply_damage`:
```c
m->last_damage_taken = 0.10f;
```

In `simulate.c::simulate_step`, after `mech_step_drive`:
```c
if (m->last_damage_taken > 0.0f) m->last_damage_taken -= dt;
if (m->last_damage_taken < 0.0f) m->last_damage_taken = 0.0f;
```

In `render.c::draw_mech`, modulate body tint:
```c
float f = m->last_damage_taken / 0.10f;
if (f > 0.0f) {
    body.r = (uint8_t)(body.r + (255 - body.r) * f);
    body.g = (uint8_t)(body.g + (255 - body.g) * f);
    body.b = (uint8_t)(body.b + (255 - body.b) * f);
}
```

Whole-mech flash, not per-particle. Cheap.

### Task 2 — Persistent damage decals

Per `documents/m5/12-rigging-and-damage.md` §"Layer 2":

Add to `Mech`:

```c
typedef struct {
    int8_t  local_x, local_y;
    uint8_t kind;       // 0=dent, 1=scorch, 2=gouge
    uint8_t reserved;
} MechDamageDecal;
#define DAMAGE_DECALS_PER_LIMB 16
typedef struct { MechDamageDecal items[DAMAGE_DECALS_PER_LIMB]; uint8_t count; } MechLimbDecals;

MechLimbDecals damage_decals[MSP_COUNT];   // per visible part
```

In `mech_apply_damage`, compute the hit's sprite-local position from the bone segment + the part's pivot, append to the right limb's ring (overflow oldest):

```c
Vec2 part_local = world_to_part_local(hit_pos_world, m, part_id);
ring->items[ring->count++ % DAMAGE_DECALS_PER_LIMB] = (MechDamageDecal){
    .local_x = clamp_i8(part_local.x), .local_y = clamp_i8(part_local.y),
    .kind = damage_kind_from_weapon(weapon_id),
};
```

In `render.c::draw_mech`, after drawing each limb sprite, composite the limb's decals on top in part-local space:

```c
for (int i = 0; i < ring->count && i < DAMAGE_DECALS_PER_LIMB; ++i) {
    const MechDamageDecal *d = &ring->items[i];
    Vec2 world_pos = part_local_to_world(d->local_x, d->local_y, m, part_id);
    DrawTexturePro(g_hud_atlas, decal_kind_src_rect(d->kind), ..., world_pos, ...);
}
```

The 5 decal sprites (3 dent + 2 scorch + gouge) live in the HUD atlas (P13).

### Task 3 — Stump caps on dismemberment

Per `documents/m5/12-rigging-and-damage.md` §"Dismemberment visuals":

In `render.c::draw_mech`, after walking limb sprites:

```c
if (m->dismember_mask & LIMB_L_ARM) {
    Vec2 sho = particle_render_pos(p, b + PART_L_SHOULDER, alpha);
    float angle = mech_torso_angle(w, m);
    draw_sprite(set->parts[MSP_STUMP_SHOULDER_L], sho, angle, body_tint);
}
/* same for R_ARM, L_LEG, R_LEG, HEAD */
```

Stump cap sprites are in the chassis atlas (5 per chassis, hand-drawn by P15/P16).

### Task 4 — Heavy initial spray + pinned stump emitter

In `mech.c::mech_dismember`, after deactivating constraints:

```c
Vec2 joint_pos = mech_joint_pos_for_limb(w, mech_id, limb);
for (int i = 0; i < 64; ++i) {
    float angle = pcg32_float01(w->rng) * 6.283185f;
    Vec2 vel = { cosf(angle) * 220.0f, sinf(angle) * 280.0f };
    fx_spawn_blood(&w->fx, joint_pos, vel, w->rng);
}
fx_spawn_stump_emitter(&w->fx, mech_id, limb, /*duration_s*/1.5f);
```

`fx_spawn_stump_emitter` is a new FX kind (FX_STUMP) per `documents/m5/12-rigging-and-damage.md`. Each tick of `fx_update`, the emitter spawns 1–2 blood particles at the *parent particle* position (look up the particle each tick — it tracks the body's motion). Expires after `duration_s`. ~50 LOC in `particle.c`.

### Task 5 — `fx_spawn_smoke` + per-limb threshold

Per `documents/m5/12-rigging-and-damage.md` §"Layer 3":

`fx_spawn_smoke` mirrors `fx_spawn_blood` with darker color, longer life, alpha decay.

In `simulate.c`, after `mech_step_drive`:

```c
for (int mi = 0; mi < w->mech_count; ++mi) {
    Mech *m = &w->mechs[mi];
    if (!m->alive) continue;
    if ((w->tick % 8) != 0) continue;
    struct { float hp, max; int part; } limbs[5] = {
        { m->hp_arm_l, 80, PART_L_ELBOW }, { m->hp_arm_r, 80, PART_R_ELBOW },
        { m->hp_leg_l, 80, PART_L_KNEE  }, { m->hp_leg_r, 80, PART_R_KNEE  },
        { m->hp_head,  50, PART_HEAD    },
    };
    for (int k = 0; k < 5; ++k) {
        float frac = limbs[k].hp / limbs[k].max;
        if (frac >= 0.30f) continue;
        float intensity = (0.30f - frac) / 0.30f;
        intensity *= intensity;
        if (pcg32_float01(w->rng) > intensity) continue;
        Vec2 src = particle_pos(&w->particles, m->particle_base + limbs[k].part);
        Vec2 vel = { (pcg32_float01(w->rng) - 0.5f) * 30.0f,
                     -20.0f - pcg32_float01(w->rng) * 30.0f };
        fx_spawn_smoke(&w->fx, src, vel, w->rng);
    }
}
```

Heavy damage: ~7 puffs/s; near-death: ~3-4/s. Squaring keeps light damage silent.

## Done when

- `make` builds clean.
- Landing a hit on a mech: visible white flash for ~100 ms.
- Repeatedly hitting the same limb: persistent dent decals accumulate on that limb's sprite.
- Dismembering a limb: heavy blood spray; stump cap visible at the joint; pinned emitter drips for ~1.5 s while the body keeps moving.
- A limb at <30% HP: visible continuous smoke trail.
- A limb at <10% HP: dense smoke plume.
- The capsule fallback (no atlas): hit-flash and damage decals still work; stump caps don't render (no sprites for them).

## Out of scope

- Per-particle hit-flash. Whole-mech is sufficient.
- Damage state sprite swap (pristine/damaged/heavy). Decal overlay does the job.
- Limb HP bars in HUD. Communication via existing kill feed + visible damage layers.
- Per-victim blood color variation. Three-color palette per the canon.

## How to verify

```bash
make
./soldut --shot tests/shots/m5_dismember.shot
```

Write `tests/shots/m5_dismember.shot` that spawns a dummy, fires repeatedly at one limb until dismemberment, captures the sequence. Verify: hit-flash on each shot, decals accumulate, dismemberment fires the heavy spray + stump cap, smoke afterward.

## Close-out

1. Update `CURRENT_STATE.md`: damage feedback layers in.
2. Update `TRADE_OFFS.md`:
   - **Delete** "Mechs rendered as raw capsules" (assuming P15/P16 has produced at least one atlas; if not yet, leave the entry with a note).
   - **Add** "Whole-mech hit flash, not per-particle" (pre-disclosed).
   - **Add** "Decal-overlay damage, not sprite-swap damage states" (pre-disclosed).
3. Don't commit unless explicitly asked.

## Common pitfalls

- **Decal local coords are i8** (-127 to +127). Sprites > 256 px tall would overflow. We don't have any; document the constraint.
- **Decal ring overflow**: when count exceeds 16, oldest gets overwritten. Don't accidentally lose the count modulus.
- **`fx_spawn_stump_emitter` particle pinning**: each tick the emitter looks up the parent particle; if the mech_id is invalid (corpse despawned), the emitter must self-deactivate.
- **Smoke + blood + spark all share `FxPool`**: a spew event can saturate the pool. Accept; pool overflow drops oldest gracefully.
- **Hit-flash modulates the body color but not weapon/decal/etc.** — only the limb sprites, not overlays.
- **Damage decal projection requires `part_local_to_world`** that uses the bone segment + pivot. Reuse `weapon_muzzle_world` shape.
