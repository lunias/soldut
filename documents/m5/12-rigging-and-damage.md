# M5 — Rigging, dismemberment & damage feedback

The art has to live on the ragdoll. The mech is built on a 16-particle Verlet skeleton, every bone is a distance constraint, dismemberment is a constraint going inactive, and the same physics runs alive and dead. **There are no static sprites in this game** — every visible part of every mech is anchored to a moving particle pair, redrawn every frame with the bone's current angle.

This doc specifies the per-part sprite anatomy that makes that work: the overlap zones that hide joint gaps, the pivot data the renderer needs, the stump-cap and exposed-end discipline that makes dismemberment look authored rather than glitchy, and the damage-feedback layer (hit-flash, persistent dents, smoke from broken limbs) that makes hits feel impactful.

The art-side production pipeline that produces these sprites is in [11-art-direction.md](11-art-direction.md); the runtime path that draws them is the existing `src/render.c::draw_mech` extended per [08-rendering.md](08-rendering.md). This doc is the contract between those two — the *shape* of the assets and the *moves* the renderer makes with them.

## Why this is its own document

A first pass at M5 specified "atlas-per-chassis with N parts." That covers asset packaging but not the load-bearing details that actually make a 2D ragdoll mech read as authored:

- Soldat and Hardcore Mecha don't *just* draw sprites along bones. They overlap them deliberately to hide gaps, and that overlap is **part of the sprite shape, not a renderer trick**.
- Dismemberment isn't "stop drawing the bone" — there's a stump cap that appears, an exposed-end that becomes visible on the detached limb, and a blood emitter that pins to the *parent* particle (so it stays with the still-moving torso, not the tumbling severed limb).
- Damage feedback that doesn't multiply asset count by 3× lives in the renderer (decal overlays, hit-flash, smoke), not in the source sprites.

Every one of these decisions is small individually. Together they're what separate "looks like Soldat" from "looks like a hobby project."

## Per-chassis bone structures and silhouettes

The design canon in [03-physics-and-mechs.md](../03-physics-and-mechs.md) Pillar 1 says **one physics model per game, parameterized**. We hold that line — every chassis uses the same 16-particle skeleton with the same constraint topology — but we use the parameterization aggressively. **Each chassis ships visibly distinct bone lengths**, drives a different sprite atlas built for those lengths, and gets a chassis-specific posture choice that emphasizes its identity.

The M4 build technically supports per-chassis bone lengths (`Chassis.bone_arm`, `bone_forearm`, `bone_thigh`, `bone_shin`, `torso_h`, `neck_h` in `src/mech.h`) but ships values that are barely distinct — at `src/mech.c:24-78`, Engineer is *identical* to Trooper, and Sniper differs by 1 px on the forearm. M5 ships an aggressive distinctness pass:

| Chassis | arm | forearm | thigh | shin | torso | neck | Stand height | Identity |
|---|---|---|---|---|---|---|---|---|
| **Trooper** (baseline) | 14 | 16 | 18 | 18 | 30 | 14 | ~80 px | Balanced, neutral. The reference silhouette. |
| **Scout** | 11 | 13 | 14 | 14 | 24 | 12 | **~67 px** | Distinctly smaller (-16%). Thin everywhere. The "small fast thing." |
| **Heavy** | 17 | 18 | 20 | 20 | **38** | 16 | **~94 px** | Distinctly larger. Barrel-chested torso (38 vs 30 = +27%). The "tank." |
| **Sniper** | 13 | **19** | 17 | **21** | 30 | 16 | ~91 px | Long forearms (rifle reach), tall stance, longer neck so the head lolls forward into a sniper hunch. |
| **Engineer** | 14 | **14** | 16 | 18 | 32 | 13 | ~78 px | Short forearms, slightly broader torso. Compact stocky utility build. |

Bone-length changes ripple naturally through the existing physics: `mech_create_loadout` reads the chassis's bone lengths and sets distance-constraint rest lengths accordingly. Pose targets in `mech_step_drive` already compute body anchor points from these fields (e.g., `src/mech.c:405` uses `chain_len = ch->bone_thigh + ch->bone_shin`). The sprite atlases are sized to match — a Heavy's atlas has a 38-px torso sprite where the Trooper's is 30 px.

### Posture differences per chassis

Beyond bone lengths, each chassis gets a **chassis-specific pose offset** applied during `build_pose` in `src/mech.c`. Same pose system, per-chassis bias values:

| Chassis | Posture quirk |
|---|---|
| Trooper | None. Neutral upright. |
| Scout | Forward lean (chest pose target offset by `+2 px x` toward facing direction). Reads as "perpetually about to dash." |
| Heavy | Locked upright (chest pose strength bumped from 0.7 → 0.85 to resist Verlet sag). Reads as "rigid power-armor stance." |
| Sniper | Head pose target offset by `+3 px y` (downward lean) and `+2 px x` toward facing. Reads as "sighting down a long barrel." |
| Engineer | None — but the `build_pose` path skips the right-arm aim drive when the secondary slot is active (so the engineer's right hand looks like it's holding a tool, not a rifle, when the BTN_SWAP is set). |

### Visual identity per chassis (sprite-level, not bone-level)

The chassis sprite atlas captures the rest of the visual identity. These are *sprite-level* differences that don't affect physics:

- **Trooper**: medium-thickness panels, neutral pauldrons. The "you've seen this before" silhouette.
- **Scout**: thin plates, low-profile shoulder caps (NOT pauldrons), visible jet vents on the forearms (they BTN_DASH a lot). Helmet has a single small visor.
- **Heavy**: blocky thick armor, **oversized pauldrons** (extend ~50% past the upper-arm bone — covers more overlap, accommodates hits at any angle). Blocky one-piece helmet. Visible exhaust vents on the back of the chest.
- **Sniper**: tall slim profile, **visor-style helmet with a horizontal mounted optic** (small sprite element at the head, visible from any aim angle). Forward-leaning chest line.
- **Engineer**: compact stocky build, **tool-belt strapping** texture on the hip plate, **shoulder-mounted welder** (small pipe sprite on one shoulder; visual only).

Each chassis's atlas sprite art reflects its bone length proportions: a Heavy's torso sprite is wider AND taller than a Trooper's, fitting the +27% torso bone. Atlases are not interchangeable — drawing Heavy bones with Trooper sprites would underfill the larger torso sub-rect.

### What we don't do (and why)

We **do not** ship per-chassis particle counts. No back-pack particle on Heavy, no forearm-mounted-optic particle on Sniper, no second-arm particle on Engineer's tool-arm. Reasons:

- The dismemberment system is built on a fixed `LIMB_*` mask broadcast over the wire (`src/world.h:83-92`). Adding a per-chassis particle would require per-chassis mask layouts, per-chassis dismemberment rules, and per-chassis snapshot fields. Big architectural change.
- The lag-comp history (`LAG_HIST_TICKS` × `PART_COUNT` per mech) assumes fixed `PART_COUNT`. Per-chassis count breaks the snapshot/lag pipeline.
- The constraint pool is sized for the worst case across all chassis. Per-chassis topology means the worst-case grows.

If a future chassis wants extra articulation (a tail, a back-mounted railgun on a turret particle, etc.) we revisit. For M5: bone-length + sprite-art distinctness is enough to make the 5 chassis read instantly different at gameplay distances.

## Sprite anatomy: rigid skinning + overlap zones

Every visible part of a mech is **one sprite anchored to one bone segment**. We don't do mesh deformation, vertex weighting, or IK chains — the canon ([03-physics-and-mechs.md](../03-physics-and-mechs.md) Pillar 2) is **single-bone-per-polygon rigid skinning**.

What does "rigid skinning" actually look like in pixels:

```
The renderer's per-part call (already present in src/render.c, expanded for M5):

    void draw_part(MechSpritePart sp, Vec2 head, Vec2 tail, ...) {
        Vec2  mid   = midpoint(head, tail);
        float angle = atan2(tail.y - head.y, tail.x - head.x);
        DrawTexturePro(atlas,
                       sp.src_rect,                       // sub-rect in atlas
                       (Rectangle){mid.x, mid.y,          // dest in world space
                                   sp.draw_w, sp.draw_h},
                       sp.pivot,                          // pivot inside src
                       angle,
                       tint);
    }
```

The bone is `[head_particle, tail_particle]`. The sprite is positioned at the midpoint, rotated by the bone's atan2 angle, tinted by hit-flash. The pivot tells `DrawTexturePro` *where on the sprite* the rotation pivots — so a head sprite pivots at its neck, an upper-arm sprite pivots roughly at its shoulder, etc.

### Overlap zones — the load-bearing trick

A naively drawn arm sprite ends exactly at the elbow. When the elbow flexes, the upper-arm and forearm sprites separate at exactly the elbow particle, and you see a one-pixel gap between them. This is the single most common 2D-ragdoll-art failure.

**The fix**: every limb sprite has its **parent end drawn ~20–25% past the joint**. The shoulder plate (drawn after the upper arm in z-order) covers that 20% overlap. When the arm bends, the upper arm's "shoulder end" is *under* the shoulder plate at any reasonable rotation; the joint never shows a gap.

Concrete sizes for the Trooper chassis (M5 reference; other chassis scale linearly):

| Part | Bone length (rest, px) | Sprite size (px) | Parent-end overlap (px) | Why |
|---|---|---|---|---|
| Upper arm | 24 | 32×80 | 14 (at shoulder side) | Shoulder plate hides this overlap |
| Forearm | 24 | 28×72 | 12 (at elbow side) | Upper arm's elbow-end hides this overlap |
| Hand | 8 | 16×16 | 4 | Forearm's wrist-end hides this overlap |
| Upper leg | 32 | 36×96 | 16 (at hip side) | Hip plate hides this overlap |
| Lower leg | 32 | 32×88 | 14 (at knee side) | Upper leg's knee-end hides this overlap |
| Foot | 12 | 32×24 | 6 | Lower leg's ankle-end hides this overlap |
| Torso (chest→pelvis) | 40 | 56×72 | 18 (top) + 14 (bottom) | Shoulder plate + hip plate hide both ends |
| Head | 16 (neck length) | 40×40 | 8 | Torso's neck-end hides this overlap |
| Shoulder plate L/R | n/a (no bone — drawn at shoulder particle) | 56×40 | n/a (it IS the cover) | Hides upper-arm overlap |
| Hip plate | n/a (drawn at pelvis particle) | 64×36 | n/a | Hides upper-leg overlap |

The shoulder plate and hip plate **don't have a bone segment** — they're drawn at the shoulder/pelvis particle position with the body's facing direction (not a bone angle). Same for the head's "back-of-helmet" silhouette — pivot at the neck, angle from neck→head, but the sprite extends past the head particle to give a full head silhouette.

### Exposed ends — the dismemberment trick

Every limb sprite has a **ragged or torn edge baked into its parent-side end** — the area normally hidden under the parent plate. This is the "wound" you see only after dismemberment. No sprite swap on detachment; the same sprite continues to draw, but now its torn edge is visible because the parent plate is gone.

Concretely: the upper-arm sprite's top 10–14 px (its overlap region) shows a torn metal silhouette with exposed wiring/hydraulic line cross-sections. Under normal play, the shoulder plate's z-order draws on top of it and you never see it. When the L_SHOULDER↔L_ELBOW constraint goes inactive (limb dismembered), the shoulder plate isn't drawn (it's the *parent's* sprite, but the constraint that anchors it to the body is what the renderer checks — see "What the renderer skips" below), so the upper arm's torn edge is visible.

This is what Soldat does and what Carrion does. It costs **zero extra sprites** for the limb side.

### Pivot specification

Each `MechSpritePart` carries its pivot in pixels relative to its sub-rect. The pivot is the point on the sprite that aligns to the bone segment's midpoint (for limb sprites) or to the parent particle (for plates).

Why pivot-relative-to-sub-rect, not pivot-as-fraction: the sprite art might be redrawn at a different aspect ratio later; pixel coordinates survive that without recalculation.

```c
// src/mech_sprites.h — new header (one of two new files this milestone)
#pragma once
#include "raylib.h"
#include "mech.h"

typedef struct {
    Rectangle src;          // sub-rect in chassis atlas (px)
    Vector2   pivot;        // pivot in src-relative px
    float     draw_w;       // world-space draw width (rest)
    float     draw_h;       // world-space draw height (rest)
} MechSpritePart;

typedef enum {
    MSP_TORSO = 0,
    MSP_HEAD,
    MSP_HIP_PLATE,
    MSP_SHOULDER_L, MSP_SHOULDER_R,
    MSP_ARM_UPPER_L, MSP_ARM_UPPER_R,
    MSP_ARM_LOWER_L, MSP_ARM_LOWER_R,
    MSP_HAND_L, MSP_HAND_R,
    MSP_LEG_UPPER_L, MSP_LEG_UPPER_R,
    MSP_LEG_LOWER_L, MSP_LEG_LOWER_R,
    MSP_FOOT_L, MSP_FOOT_R,
    /* Stump caps — drawn over the parent particle when the limb is detached. */
    MSP_STUMP_SHOULDER_L, MSP_STUMP_SHOULDER_R,
    MSP_STUMP_HIP_L,      MSP_STUMP_HIP_R,
    MSP_STUMP_NECK,
    MSP_COUNT
} MechSpriteId;

typedef struct {
    Texture2D       atlas;
    MechSpritePart  parts[MSP_COUNT];
} MechSpriteSet;

extern MechSpriteSet g_chassis_sprites[CHASSIS_COUNT];

bool mech_sprites_load_all(void);
void mech_sprites_unload_all(void);
```

The `g_chassis_sprites` array is populated at startup by reading per-chassis atlas PNGs from `assets/sprites/<chassis>.png` and the per-chassis sub-rect/pivot table from `assets/sprites/<chassis>.atlas` (a small text file produced by rTexPacker — see [11-art-direction.md](11-art-direction.md) §"Workflow management"). At v1 we transcribe rTexPacker's JSON into a static `g_chassis_sprites` table by hand (~50 minutes one-time work for 5 chassis × 22 entries), per the canon Rule 6 ("no codegen in v1"). v2 may add a tiny `.atlas` parser if the table grows tedious.

## Per-weapon visible art

The M4 build draws the same generic line for every weapon at `src/render.c:226-235`:

```c
/* Rifle: a small line from R_HAND in aim direction... */
Vec2 muzzle = { rh.x + dx * 22.0f, rh.y + dy * 22.0f };
draw_bone_clamped(L, rh, muzzle, 3.0f, (Color){50, 60, 80, 255});
```

A Sidearm and a Mass Driver render identically — a thin grey line. M5 replaces this with **a per-weapon sprite atlas** so each of the 14 weapons has its own visible silhouette in the mech's hand.

### Weapon sprite anatomy

A weapon sprite is anchored at the mech's **R_HAND particle**, rotated along the **aim direction** (not the bone direction; the weapon's *grip* sits at the hand and its *muzzle* extends along the player's aim ray, not the forearm bone). Each weapon's sprite carries three pieces of metadata:

```c
// src/weapon_sprites.h — new module
typedef struct {
    Rectangle src;              // sub-rect in weapons.png atlas (px)
    Vector2   pivot_grip;       // grip position in src-relative px (where R_HAND attaches)
    Vector2   pivot_foregrip;   // foregrip position in src-relative px; (-1,-1) for one-handed
    Vector2   muzzle_offset;    // muzzle position in src-relative px (where shots emit visually)
    float     draw_w, draw_h;   // world-space size at rest
    int       weapon_id;        // WeaponId — for table lookup
} WeaponSpriteDef;

extern const WeaponSpriteDef g_weapon_sprites[WEAPON_COUNT];
extern Texture2D g_weapons_atlas;

const WeaponSpriteDef *weapon_sprite_def(int weapon_id);
bool weapons_atlas_load(void);
void weapons_atlas_unload(void);
```

### Per-weapon visible sizes

Each weapon ships at its visually-correct size. Sizes are starting targets; tune in playtest.

| Weapon | Sprite size (px) | Two-handed | Visible identity |
|---|---|---|---|
| **Pulse Rifle** | 56×14 | yes | Compact rifle, side-mounted optic |
| **Plasma SMG** | 48×16 | yes | Chunky energy SMG, glowing barrel |
| **Riot Cannon** | 60×24 | yes | Wide shotgun-style; shell-loading visible |
| **Rail Cannon** | **88×16** | yes | Long thin rifle; coil rings along the barrel |
| **Auto-Cannon** | 60×16 | yes | Full-size assault rifle |
| **Mass Driver** | **96×32** | yes | Huge rocket launcher; the longest weapon |
| **Plasma Cannon** | 64×22 | yes | Thick energy launcher; visible plasma chamber |
| **Microgun** | 80×28 | yes | Multi-barrel; visibly chunky |
| **Sidearm** | **28×10** | no | Tiny pistol — the smallest weapon |
| **Burst SMG** | 44×14 | yes (small foregrip) | Compact SMG with stubby foregrip |
| **Frag Grenades** | 16×16 | no | Hand-held grenade; visible only when held / thrown |
| **Micro-Rockets** | 52×20 | yes | Side-mounted rocket pod |
| **Combat Knife** | 32×8 | no | Short blade; held below R_HAND |
| **Grappling Hook** | 36×14 | no | Compact wrist-mounted launcher |

A Heavy holding a Mass Driver fills nearly the entire height of the mech with weapon. A Scout with a Sidearm is barely visibly armed. **This silhouette difference is load-bearing for tactical readability** — you can tell at 600 px what someone is shooting with.

### Atlas packaging

All 14 weapon sprites pack into a single shared atlas:

```
assets/sprites/weapons.png    # 1024×512 RGBA8
assets/sprites/weapons.atlas  # rTexPacker-style sub-rect + pivot table (transcribed to C)
```

~512 KB on disk, ~2 MB in RAM. One atlas binding per frame for all weapon draws across all mechs. Inside [10-performance-budget.md](../10-performance-budget.md) texture cap.

### Render path

```c
// src/render.c — replaces the line-rifle code
static void draw_held_weapon(const ParticlePool *p, const Mech *m, const Level *L) {
    if (!m->alive) return;
    const WeaponSpriteDef *wp = weapon_sprite_def(m->weapon_id);
    if (!wp) return;

    int b = m->particle_base;
    Vec2 rh   = particle_pos(p, b + PART_R_HAND);
    Vec2 aim  = mech_aim_dir(/*world*/ ...);   // unit vector toward cursor
    float angle = atan2f(aim.y, aim.x) * RAD2DEG;

    DrawTexturePro(g_weapons_atlas,
                   wp->src,
                   (Rectangle){rh.x, rh.y, wp->draw_w, wp->draw_h},
                   wp->pivot_grip,         // pivot in src-relative px
                   angle,
                   m->is_dummy ? (Color){200,130,40,255} : WHITE);

    // Optional: muzzle-flash sprite at wp->muzzle_offset, when m->fire_cooldown
    // is fresh (< 50 ms ago). Drawn additive over the weapon.
    if (m->fire_cooldown > wp->fire_rate_sec - 0.05f) {
        Vec2 muzzle_world = weapon_muzzle_world(rh, aim, wp);
        draw_muzzle_flash(muzzle_world, angle);
    }
}
```

The pivot at `wp->pivot_grip` aligns the weapon's grip with the mech's R_HAND. The aim angle rotates the weapon along the aim ray. Behind that, the muzzle ends up exactly where bullets/projectiles spawn — `weapons.c::weapons_fire_hitscan` already computes muzzle position from `hand + dir * wpn->muzzle_offset`; M5 changes the muzzle_offset to come from the sprite metadata so visible muzzle and physical muzzle coincide.

### What this fixes

- **Visible weapon identity** — Mass Driver looks like a Mass Driver; Sidearm looks like a Sidearm.
- **Visible muzzle position** — bullets emit from the visible end of the weapon, not from somewhere arbitrary 22 px past R_HAND. Improves the feel of every shot.
- **Replaces the M4 generic line** — the existing `draw_bone_clamped` from R_HAND→muzzle goes away. The wall-clamp logic that prevents the weapon barrel from drawing through walls is preserved by adding a per-weapon ray-clamp (the weapon sprite gets clipped at the first solid tile crossing).

## Two-handed weapons and the off-hand foregrip

The M1 trade-off "Left arm has no pose target" produces the visible "left hand dangles" issue when the mech holds a two-handed rifle. The user sees a rifle being held by one hand with the other hand swinging awkwardly.

M5 addresses this **for two-handed weapons only** by driving the L_HAND pose target to the weapon's foregrip position:

```c
// src/mech.c::build_pose — added when active weapon is two-handed
const WeaponSpriteDef *wp = weapon_sprite_def(m->weapon_id);
if (wp && wp->pivot_foregrip.x >= 0.0f) {
    /* Compute foregrip world position. */
    Vec2 rh  = particle_pos(p, b + PART_R_HAND);
    Vec2 aim = mech_aim_dir(world, mech_id);
    float perp_x = -aim.y, perp_y = aim.x;
    /* The foregrip offset in sprite-local space: along-aim + perpendicular. */
    float along = wp->pivot_foregrip.x - wp->pivot_grip.x;     // sprite-local px
    float perp  = wp->pivot_foregrip.y - wp->pivot_grip.y;
    Vec2 foregrip = {
        rh.x + aim.x * along + perp_x * perp,
        rh.y + aim.y * along + perp_y * perp,
    };
    /* Drive L_HAND pose target. */
    pose->target_pos[PART_L_HAND] = foregrip;
    pose->strength[PART_L_HAND]    = 0.6f;
}
```

For one-handed weapons (Sidearm, Frag Grenades, Combat Knife, Grappling Hook), `pivot_foregrip = (-1, -1)` and the L_HAND pose stays unset — the off-hand dangles freely (the M1 default). Visually correct: a sidearm doesn't get held two-handed.

The constraint solver then pulls the L_ELBOW and L_SHOULDER toward sensible IK-like positions automatically, since the L_HAND is being yanked toward the foregrip. This is **rough IK by way of constraint relaxation** — not analytically perfect but visually strongly preferable to the dangling arm.

This **partially resolves** the M1 trade-off "Left arm has no pose target / no IK." Trade-offs entry can be amended to: "Left arm has no IK *for one-handed weapons*; for two-handed weapons the foregrip drives L_HAND naturally." The full IK solver remains a stretch goal.

### Foregrip positions per two-handed weapon

The atlas table specifies foregrip per weapon. Sprite-local pixels (origin at the sprite's top-left in the atlas):

| Weapon | grip x,y | foregrip x,y | muzzle x,y |
|---|---|---|---|
| Pulse Rifle | 8, 7 | 32, 7 | 56, 7 |
| Plasma SMG | 6, 8 | 24, 8 | 48, 8 |
| Riot Cannon | 8, 14 | 28, 14 | 60, 14 |
| Rail Cannon | 8, 8 | 32, 8 | 88, 8 |
| Auto-Cannon | 8, 8 | 28, 8 | 60, 8 |
| Mass Driver | 12, 18 | 40, 18 | 96, 16 |
| Plasma Cannon | 8, 12 | 28, 12 | 64, 12 |
| Microgun | 10, 14 | 32, 14 | 80, 14 |
| Burst SMG | 6, 7 | 22, 7 | 44, 7 |
| Micro-Rockets | 8, 10 | 26, 10 | 52, 10 |

One-handed: Sidearm, Frag Grenades, Combat Knife, Grappling Hook all have `foregrip = (-1, -1)`.

### How this looks in motion

When a Trooper-with-Pulse-Rifle aims, the right arm extends along the rifle, the left arm bends into the foregrip — both hands on the gun, classic combat-aim pose. As they swing the cursor, both hands track the aim direction; the pose is right-hand-anchored, left-hand-foregrip-anchored, and the arms naturally bend through their constraint chains.

When the same Trooper switches to Sidearm (BTN_SWAP), the left arm drops — pose target is unset, the constraint chain hangs naturally. Visible state-change feedback for the slot swap.

When the same Trooper presses BTN_FIRE_SECONDARY (RMB; see [13-controls-and-residuals.md](13-controls-and-residuals.md)) to throw a grenade *without* swapping slots: the right hand briefly switches its rifle for a grenade for one tick (the throw animation), then back to the rifle. This is a render-only effect — for the throw frame, `weapon_sprite_def(WEAPON_FRAG_GRENADES)` is drawn instead of the rifle, then it reverts. The off-hand foregrip stays on the rifle; the secondary fire is one-shot through the throwing hand.

## Render order

Within one mech, parts are drawn in this order. Soldat does the same; Spine documents the same canonical pattern.

```
1. back leg upper      ← back of body, drawn first
2. back leg lower
3. back foot
4. back arm upper
5. back arm lower
6. back hand
7. torso (chest → pelvis)
8. hip plate           ← covers upper-leg overlap on both sides
9. shoulder plate L+R  ← covers upper-arm overlap on both sides (back & front shoulders)
10. head
11. front leg upper
12. front leg lower
13. front foot
14. front arm upper
15. front arm lower
16. front hand
17. weapon (held by R_HAND)
```

"Back" vs "front" depends on `m->facing_left` — when facing left, the L_* limbs are visually behind the body; when facing right, the R_* limbs are. The existing code at `src/render.c:168-173` already does this swap.

**The order does NOT change mid-tumble.** When a mech dies and the body rotates, technically the back arm should sometimes draw in front. Shipped 2D ragdoll games (Soldat included) tolerate the brief weirdness because the body's dead. We tolerate it too. (See "Ragdoll-mode visual continuity" below.)

## Dismemberment visuals

When a limb's HP hits zero, `mech_dismember` in `src/mech.c` deactivates the distance constraints anchoring the limb to the parent. The renderer responds:

### What the renderer skips

- The bone segment for the inactive constraint isn't drawn. Existing helper `bone_constraint_active` at `src/render.c:105` already does this.
- The **parent plate** (shoulder plate for arm dismemberment, hip plate for leg dismemberment) is NOT skipped — the plate still belongs to the torso, the constraint that's gone is *between the plate and the limb*. The plate continues to draw.
- The detached limb's particles are **still in the global pool, still integrating, still subject to gravity and tile collision**. The renderer continues to walk the limb's bone segments — the limb's *internal* constraints (upper-arm to forearm, forearm to hand) are still active.

### What the renderer adds

- The **stump cap** sprite for the detached joint is drawn at the parent particle. The stump cap shows the inside of a torn-off mech limb — torn metal, exposed cables, hydraulic line cross-sections, fluid drip. Drawn at the parent particle, oriented to the body's facing direction.
- The **exposed end** of the detached limb is now visible because the parent plate that previously covered its overlap region has dropped one z-layer. (Actually it didn't — the stump cap z-orders just under the parent plate. The exposed end is visible because the limb's overlap region used to extend INTO the area covered by the parent plate, and now the limb has separated from that area, so the previously-overlapped pixels are no longer overlapped.)

```c
// src/render.c — pseudo-code addition to draw_mech
if (m->dismember_mask & LIMB_L_ARM) {
    Vec2 sho = particle_pos(p, b + PART_L_SHOULDER);
    float body_angle = mech_torso_angle(w, m);
    draw_sprite(set->parts[MSP_STUMP_SHOULDER_L], sho, body_angle, tint);
}
if (m->dismember_mask & LIMB_R_ARM) { /* mirror */ }
if (m->dismember_mask & LIMB_L_LEG) {
    Vec2 hip = particle_pos(p, b + PART_L_HIP);
    draw_sprite(set->parts[MSP_STUMP_HIP_L], hip, body_angle, tint);
}
if (m->dismember_mask & LIMB_R_LEG) { /* mirror */ }
if (m->dismember_mask & LIMB_HEAD) {
    Vec2 nck = particle_pos(p, b + PART_NECK);
    draw_sprite(set->parts[MSP_STUMP_NECK], nck, body_angle, tint);
}
```

5 stump cap sub-rects per chassis × 5 chassis = 25 small sprites total. Each is ~32×32 px in the atlas. About 2.5% of the atlas budget.

### The blood/oil emitter

When `mech_dismember` fires, spawn a heavy blood emitter at the joint, **pinned to the parent particle** (so it stays with the still-moving torso, not the tumbling severed limb):

```c
// src/mech.c — extend mech_dismember
void mech_dismember(World *w, int mech_id, int limb) {
    Mech *m = &w->mechs[mech_id];
    /* ... existing constraint deactivation ... */

    /* Heavy initial spray — 50–80 blood particles in a fan from the
     * joint, biased toward the limb's direction of separation. */
    Vec2 joint_pos = mech_joint_pos_for_limb(w, mech_id, limb);
    for (int i = 0; i < 64; ++i) {
        float angle = pcg32_float01(w->rng) * 6.283185f;
        Vec2 vel = { cosf(angle) * 220.0f, sinf(angle) * 280.0f };
        fx_spawn_blood(&w->fx, joint_pos, vel, w->rng);
    }

    /* Continuous low-rate emit for ~1.5 s pinned to the parent. The
     * emitter is a new pool entry so it survives across ticks; expires
     * by FxKind decay rule. */
    fx_spawn_stump_emitter(&w->fx, mech_id, limb, /*duration_s*/1.5f);
}
```

`fx_spawn_stump_emitter` is a new emitter type that, on each fx_update tick, spawns 1–2 blood particles at the parent-particle position (looking up the particle every tick so it tracks the body's motion). Expires after the duration. ~30 LOC in `particle.c`.

The emitted blood lands on the splat layer via the existing decal flow — the trail follows the dismembered body wherever it tumbles. Persistent visual record of the kill.

### Stump cap art is hand-drawn

The stump cap is a small, very specific asset (torn metal interior, exposed cables, hydraulic cross-sections) that AI generators are *bad* at — they have minimal training data for "the inside of a mech's torn-off shoulder." Per [11-art-direction.md](11-art-direction.md) §"Pipeline 5", we **hand-draw all 25 stump caps** (5 per chassis × 5 chassis). At ~30 minutes each, that's 12 hours total — well inside the milestone budget, and the assets are clean and consistent.

## Damage feedback — three layers

The user-visible damage feedback is three runtime systems layered on top of the static sprite art. None of them require additional source assets per limb (we don't ship "pristine / scuffed / heavy" sprite variants).

### Layer 1 — Hit-flash tint

The moment a damage event lands on a particle, the part's tint adds a white-flash that decays over ~80–120 ms (5–8 ticks at 60 Hz). This is the "you just landed a hit" signal that Hollow Knight, Dead Cells, and every action game since Mega Man Zero use. White-additive flash > saturate-to-white because it preserves silhouette legibility.

Implementation: per-mech `hit_flash_timer` field, set to 0.10 s in `mech_apply_damage`, decremented in simulate. Renderer modulates the body tint:

```c
// src/render.c — extend draw_mech
float f = m->hit_flash_timer / 0.10f;
if (f > 0.0f) {
    body.r = (uint8_t)(body.r + (255 - body.r) * f);
    body.g = (uint8_t)(body.g + (255 - body.g) * f);
    body.b = (uint8_t)(body.b + (255 - body.b) * f);
}
```

Whole-mech flash is the right granularity for v1 (the per-particle granularity the existing code has on `contact_kind` is for floor/wall contact, not damage). Per-limb flash is a polish item — if a player can't tell which arm got hit, the kill feed already attributes the damage.

Cost: 1 byte per Mech (a `float` repurposed; we already track `last_damage_taken`), one branch per render call.

### Layer 2 — Persistent damage decals on the part

Each damage event drops a small dent/scorch decal sprite onto a per-mech damage-overlay buffer. When the limb is rendered, the overlay quad is drawn on top of the limb sprite at the same destination rect. Decals **migrate with the limb** because they're rendered using the same bone-driven destination.

The cheap v1 implementation does NOT use a per-mech RT (which would cost ~1.5 MB across 32 mechs). Instead, it uses a **decal list per limb** — each limb keeps a small ring of `(local_x, local_y, decal_kind)` records and the renderer composites them every draw. ~16 decal records per limb × 12 limbs × 32 mechs × 8 bytes/record = 48 KB. Much cheaper than RTs.

```c
// src/world.h — added to Mech
typedef struct {
    int8_t   local_x, local_y;    // sprite-local coords (sub-pixel ok with i8 at 1× scale)
    uint8_t  kind;                // 0=dent, 1=scorch, 2=gouge
    uint8_t  reserved;
} MechDamageDecal;
#define DAMAGE_DECALS_PER_LIMB 16
typedef struct {
    MechDamageDecal items[DAMAGE_DECALS_PER_LIMB];
    uint8_t         count;        // ring; oldest aged out
} MechLimbDecals;

// In Mech:
MechLimbDecals damage_decals[MSP_COUNT];
```

On each damage event, compute the hit's position relative to the part's sprite-space (using the bone segment + the part's pivot), append to the part's decal ring. Renderer iterates and draws each decal as a single sprite quad in part-local space. Decals never get "cleared" mid-round; the ring overflow handles age-out.

5 decal sprites total in the HUD atlas (3 dent variants, 2 scorch variants), shared across all chassis. 32×32 each. Authored once.

The `kind` enum maps to:
- 0: small dent (light damage, < 25% damage on the part)
- 1: scorch mark (explosion damage)
- 2: gouge (heavy damage, > 50%)

Pick `kind` based on the damage event's weapon type (already known in `mech_apply_damage`).

### Layer 3 — Smoke from heavily damaged limbs

When a limb's HP drops below 30% of its max, the limb starts emitting smoke continuously. Quake/Half-Life pattern. Cuphead's Werner Werman tank does this beautifully in 2D.

```c
// src/simulate.c — added after mech_step_drive
for (int mi = 0; mi < w->mech_count; ++mi) {
    Mech *m = &w->mechs[mi];
    if (!m->alive) continue;
    /* Per-limb smoke check. Tick gating means we emit at most once per
     * 8 ticks (~7.5 Hz), and even less for lighter damage. */
    if ((w->tick % 8) != 0) continue;
    struct { float hp; float max; int part; } limbs[5] = {
        { m->hp_arm_l, 80.0f, PART_L_ELBOW },
        { m->hp_arm_r, 80.0f, PART_R_ELBOW },
        { m->hp_leg_l, 80.0f, PART_L_KNEE  },
        { m->hp_leg_r, 80.0f, PART_R_KNEE  },
        { m->hp_head,  50.0f, PART_HEAD    },
    };
    for (int k = 0; k < 5; ++k) {
        float frac = limbs[k].hp / limbs[k].max;
        if (frac >= 0.30f) continue;
        /* Damage intensity squared — light puffs near 30%, dense plume near 0. */
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

`fx_spawn_smoke` already exists as `FX_SMOKE` in `src/particle.c::fx_update` (the kind is reserved; the emitter function isn't written). Add it as a mirror of `fx_spawn_blood` with darker color and longer life. ~20 LOC.

Heavy damage emits ~7 puffs/s; near-death emits ~3-4/s. The squaring means light damage is essentially silent, which keeps the FX pool from saturating in mid-fight chaos.

### What we explicitly DON'T do

- **Per-limb HP bars in the HUD.** The kill feed + the visual damage layers (decals, smoke, dismemberment) cover communication. Per-limb HP bars are visual clutter.
- **Discrete damage states (sprite swap at HP thresholds).** 3× the source asset count. Decal overlays do the same job for free.
- **Per-frame blood splatter on the body sprite.** The decal RT layer (the level's persistent splat layer) catches blood that lands on geometry; that's enough.
- **Sparks from limbs.** Sparks already fire from bullet impacts (they fly off the contact point). A limb that's dismembered doesn't *itself* spark — that would be redundant.

## Ragdoll-mode visual continuity

When a mech dies, pose drive stops, the body is a free ragdoll. The same sprite-on-bone rendering still runs. Three artifacts are tolerated rather than fixed:

### Self-intersection

A dead mech's arm folds back into its torso; the sprite of one limb visibly overlaps another. Soldat tolerates this. So do we. The body's dead; nobody notices.

### Plate flip past 180°

When the arm rotates past straight-up (the bone's atan2 angle crosses ±π), the sprite's "up" direction flips relative to the body. For most mech parts this is invisible (the part is roughly symmetric top-to-bottom). For asymmetric parts (the head, the chest with its shoulder plates) it's potentially weird.

Two solutions:
- **Symmetric design** — author parts so they look the same flipped. Cheapest.
- **`flipY` on sign change** — when the bone's parent-relative angle crosses ±π/2, flip the V coordinate of the sprite's source rect. Soldat does this in `Render.pas` per-part. ~15 LOC in `draw_mech`.

For M5 we ship symmetric design (cheap). Authored intent: the upper-arm sprite has no clear top-to-bottom asymmetry; the chest sprite is left-right asymmetric (different shoulder plates) but top-to-bottom symmetric. If a part *has* to be asymmetric (the helmet has a visor), we add the flipY logic for that part only.

### Z-order changing as the body tumbles

Mid-tumble, the back arm should *briefly* draw in front of the body. The canonical render order doesn't account for this. Shipped games either re-sort within mech by Y-position per frame (Spine games, Hardcore Mecha) or tolerate the artifact (Soldat).

We tolerate. Re-sorting 17 parts per dead mech per frame is the right shape for v2 polish; v1 is "the body looks weird mid-tumble, settles fine when it sleeps."

## What the existing codebase already gets you for free

The Verlet ragdoll engine is *already the entire animation system*. Specifically you do not need to build:

- A skeletal animation runtime (Spine / DragonBones equivalent) — the pose driver IS the animator.
- Inbetweens between animation frames — the constraint solver does this.
- An explicit "ragdoll mode" toggle — pose strength = 0 IS ragdoll mode.
- Bone-to-sprite weighting — single-bone-per-polygon is canon.
- Mesh deformation, vertex skinning, IK chains.
- A constraint-deactivation handler in the renderer — `bone_constraint_active` at `src/render.c:105` already exists.
- Per-particle limb HP — already in `Mech.hp_arm_l/r`, `hp_leg_l/r`, `hp_head`.
- A dismemberment broadcast — `m->dismember_mask` already rides snapshots.
- A blood/spark/smoke FX pool — `FxPool` in `src/particle.c` already holds all three kinds.
- A persistent splat layer — `decal.c` already does this.
- A face-direction render order swap — `m->facing_left` swap at `src/render.c:168-173` already does this.

## The minimal extension

**Per-part atlas + pivots** (the bulk of the work):
- New file `src/mech_sprites.{h,c}` (~150 LOC).
- Hand-coded `g_chassis_sprites[CHASSIS_COUNT]` table populated from rTexPacker output.
- Atlas PNGs at `assets/sprites/<chassis>.png`.
- Replace `src/render.c::draw_mech` (the capsule version becomes the M4 fallback when `set->atlas.id == 0`).

**Stump caps**:
- 5 sprites per chassis added to the atlas (handled by [11-art-direction.md](11-art-direction.md) §"Pipeline 5 — Hand-drawn stump caps").
- Render-side: the dismember-mask check + `draw_sprite` calls in the new `draw_mech`. ~25 LOC.

**Heavy initial blood spray + pinned stump emitter**:
- Extend `mech_dismember` with the 64-particle spray and the `fx_spawn_stump_emitter` call. ~30 LOC.
- `fx_spawn_stump_emitter` is a new FX kind (FX_STUMP) that pin-tracks the parent particle. ~50 LOC in `particle.c`.

**Hit-flash tint**:
- Repurpose `Mech.last_damage_taken` (already exists, currently underused) as a 0–0.10 s timer.
- ~3 LOC in `mech_apply_damage`, ~5 LOC in `simulate.c` decay, ~6 LOC in `draw_mech` color modulation.

**Damage decals**:
- Add `MechLimbDecals damage_decals[MSP_COUNT]` to Mech (96 KB across all mechs — accept).
- ~30 LOC in `mech_apply_damage` to compute sprite-local hit position and append to the right limb's ring.
- ~25 LOC in `draw_mech` to composite decal sprites on top of each limb's draw.
- 5 decal sub-rects in the HUD atlas (3 dent, 2 scorch).

**Smoke from damaged limbs**:
- Add `fx_spawn_smoke` to `particle.c` (mirror of `fx_spawn_blood`). ~20 LOC.
- Add the per-limb threshold check in `simulate.c` after `mech_step_drive`. ~30 LOC.

**Total code budget**: ~370 LOC delta across 6 files. Plus the asset work in [11-art-direction.md](11-art-direction.md).

## Done when

- A fresh checkout with assets shipped renders mechs with sprite atlases instead of capsules; the M4 capsule fallback fires only when the atlas isn't loaded.
- Bending an arm at 60° shows no visible joint gap (the shoulder plate covers the overlap).
- Dismembering an arm: the parent shoulder shows a stump cap; the detached arm shows its torn-edge end; a heavy blood spray fires at the joint; a pinned blood emitter trails the parent for ~1.5 s.
- A landed shot produces a visible white-flash on the mech and leaves a persistent dent decal on the hit limb.
- A limb at <30% HP visibly emits smoke; at <10% HP the smoke is a continuous plume.
- A dead body's render artifacts (self-intersection, occasional plate-flip) are visible but not jarring; the renderer doesn't crash on any constraint configuration.
- The 5 ship chassis × 22 atlas entries each are committed to `assets/sprites/`; `g_chassis_sprites` table populated.
- A shot test (`tests/shots/m5_dismember.shot`) shows the full sequence: pristine → damaged → dismembered → ragdoll-tumble → settled corpse, all with the correct visual feedback at each beat.

## Trade-offs to log

- **Decal-overlay damage, not sprite-swap damage states.** No `pristine.png` / `damaged.png` / `heavy.png` per part. 3× asset reduction. Damage decals are 5 shared sprites total. Known gap: the *art-style* of the decal must match the chassis it lands on, which we handle with a single neutral linework decal that reads against any chassis palette.
- **Whole-mech hit flash, not per-particle.** Cheap and sufficient. Per-particle would need a separate hit-flash field per particle (12 KB across the pool); skip until playtest reveals "I can't tell which arm got hit."
- **Symmetric mech parts to avoid flip-past-180° artifact.** No `flipY` logic in v1. Some asymmetry (visor on Sniper helmet) gets per-part flipY in M6 polish.
- **No re-sort by Y mid-tumble.** Dead-body rendering can briefly look weird when limbs cross. Tolerated.
- **Decal records use sprite-local int8 coords.** ±127 px range; fine for sprites under 256 px tall (which all of ours are). If a chassis goes bigger we widen to int16.
- **Stump caps are hand-drawn, not AI-generated.** Documented in [11-art-direction.md](11-art-direction.md) §"Pipeline 5". The 12-hour budget item is the cleanest path; AI generators are bad at this specific subject.
- **No "limb HP bar" UI.** Communication via the kill feed + visible damage layers is sufficient; per-limb bars are clutter.
- **The detached limb's torn end is pre-baked into every limb sprite.** Same texture in attached and detached state; the parent plate's z-order hides it during normal play. Trade-off: every limb sprite must be drawn with this discipline (the parent end is always the torn end), which constrains the art slightly.
