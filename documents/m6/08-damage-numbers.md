# M6 — Damage Numbers

**Goal**: spawn a physics-driven, color-graded number floating out of
every mech that takes damage, on both the attacker's and the victim's
window in sync. The number flies out from the hit, falls under gravity,
bounces once or twice on the floor with a metallic **tink**, rotates as
it tumbles, fades, and disappears. Sized small so a busy 16-mech
firefight doesn't drown in floating glyphs; colored so a player can
read "how much did I just do?" at a glance without parsing digits.

Read this whole document end-to-end before opening a single file. Skip
ahead is allowed for §10 (Test plan) and §12 (Implementation phases)
once you're deep in the work.

---

## 0. Scope and non-goals

**In scope**
- A new `FxKind` value `FX_DAMAGE_NUMBER` in the existing `FxPool`,
  plus two new `FxParticle` fields (`angle`, `ang_vel`) so the glyph
  can tumble. No new pool, matching the M6 P02 convention.
- One new spawn function `fx_spawn_damage_number(pool, pos, dir,
  damage_u8, weapon_id, rng)` next to the existing `fx_spawn_*` API in
  `src/particle.{c,h}`. The damage value packs into `pin_mech_id`
  (already an `int16_t` — reused for FX_STUMP only); the bounce
  counter packs into `pin_limb` (u8); a small flags byte (crit / heavy
  tier) packs into `pin_pad`.
- One new spawn seam wired into the existing damage paths:
  - **Client-side**: `src/net.c::client_handle_hit_event` after the
    blood/sparks spawn (already at net.c:2254) — the canonical "remote
    visible visual" path. Every client gets the spawn from the server's
    HIT_EVENT broadcast, including the host's UI client via UDP
    loopback (per wan-fixes-16 the host server runs on its own
    `Game` struct on a worker thread; the UI is a separate client).
  - **Server-side**: `src/mech.c::mech_apply_damage` after the existing
    blood/sparks spawn. This path only renders visibly in
    **single-process / offline** runs (shotmode without `network host`,
    `--test-play`, future practice mode) where there's no separate UI
    thread and no broadcast loopback. In multiplayer the server's
    spawn lands in the **server thread's** `FxPool`, never rendered.
    Same pattern the existing blood/sparks spawn already uses — no new
    dedup needed.
- Per-tick physics in `src/particle.c::fx_update`:
  - Gravity (same as `FX_BLOOD`).
  - Air drag (mild: 0.99/frame).
  - Tile collision via the existing `level_point_solid` query — the
    number that blood already uses for splat-on-wall — but instead of
    dying on hit, FX_DAMAGE_NUMBER **bounces**: `vy = -vy * 0.55`,
    `vx *= 0.7`, `ang_vel *= 0.6`. Triggers one `audio_play_at` of
    a metal-tink SFX per bounce. Capped at **2 bounces**; on the third
    contact, snap to rest (clear velocities + angular velocity) and
    let the life-fade carry the glyph out.
  - Angular integration: `angle += ang_vel * dt`. Free-flying;
    bounces damp it.
  - Life decay: `life -= dt` like other FX. Alpha fades over the last
    `0.50 s`; before that the glyph holds full alpha so it's readable
    while it's still moving.
- A new render pass `damage_numbers_draw(world, renderer)` invoked
  from `src/render.c::renderer_draw_frame` **after** the
  internal-target upscale blit and **before** the HUD. This is
  deliberate (see §8): drawing in the internal RT would bilinear-blur
  the text on upscale; drawing after the blit lets the glyphs land
  at sharp window pixels. The pass walks the FxPool, picks out
  `kind == FX_DAMAGE_NUMBER`, converts each particle's world position
  to window pixels using the existing `Renderer.blit_scale/dx/dy`
  fields (the same numbers M6 P03 already publishes for the HUD
  cursor / off-screen-indicator transforms), and calls `DrawTextPro`
  with rotation + a 4-pass black outline.
- A 5-tier damage→color/size table (§9). Color goes pale-yellow →
  yellow → orange → red-orange → bright-red. Size goes
  10 px (small hit) → 18 px (heavy/lethal). All thresholds derived
  from the existing M5 P12 decal thresholds so the color tier and the
  decal tier on the limb agree visually.
- One new SFX entry `SFX_DAMAGE_TINK` keyed to the metal-bounce
  cadence — sourced from `kenney_impact-sounds/impactMetal_light_*.ogg`
  through the existing `tools/audio_inventory/source_map.sh` +
  `tools/audio_normalize/normalize.sh` pipeline (the same path M6 P02's
  jet-ignition SFX took). v1 fallback: reuse the existing
  `SFX_HIT_METAL` if the new asset isn't yet present, so the feature
  builds and is playable before the asset PR lands.
- One new shotmode directive `at TICK damage MECH PART AMOUNT` so the
  single-process color-tier shot test can exercise every tier without
  having to set up a real weapon + cooldown sequence. Sits next to the
  existing `kill_peer` directive in `src/shotmode.c`.
- A new paired-shot test `tests/shots/net/2p_damage_numbers.{host,
  client}.shot` + `tests/shots/net/run_damage_numbers.sh` that fires a
  known weapon (Pulse Rifle for the predictable 12-damage per hit) at
  a stationary peer and asserts that BOTH windows log the same spawn
  line with the same damage value. New SHOT_LOG line
  `dmgnum spawn victim=N pos=X,Y dmg=D tier=T` makes the assertion
  trivial to grep.
- A new single-process shot test `tests/shots/m6_damage_number_tiers.shot`
  exercising the 5 color tiers via the new `damage` shotmode directive,
  with a `contact_sheet` composite that visually validates the color
  ramp.
- Documentation: `CURRENT_STATE.md`, `CLAUDE.md` status line, the
  status footnote at the end of THIS document.

**Out of scope**
- No wire-format changes. `NET_MSG_HIT_EVENT` already carries
  (victim_id, hit_part, pos_x_q, pos_y_q, dir_x_q, dir_y_q, damage_u8)
  per net.c:3118-3159. Every byte the spawn function needs is already
  on the wire. **Protocol id stays `S0LK`.** (If a future tier wants
  per-weapon icons or stacked dmg-over-time displays, that's another
  PR.)
- Not a damage-stacking system. Five hits in 100 ms produce five
  separate flying numbers. Stacking like Diablo's "+12, +24, +36"
  combine-into-one is a different feature with a different feel — see
  §15 risk table for the "visual clutter" trade-off and the deliberate
  reason we don't stack at v1.
- No screen shake driven by damage numbers. The existing recoil/bink
  systems already shake the camera; the number is a feedback layer on
  top, not another shake source.
- No floating-text for healing pickups at v1. The HUD already shows the
  health bar pop. A green "+25" on health-pack pickup is a 30-line
  future addition that reuses the same render path; not in this scope.
- No per-damage-type color variants (fire/cryo/plasma). All damage
  uses the same yellow → red ramp keyed off magnitude. Adding type
  colors is a 10-line table extension when (if) elemental damage ships.
- No new pool. The existing `FxPool` capacity (`MAX_BLOOD = 8000`)
  has more than enough headroom — the worst-case sim is 16 mechs ×
  10 dmg/s × ~2 s lifetime ≈ 320 simultaneous numbers, well under the
  ~320 headroom slots remaining after the M6 P02 jet-particle budget.
- No font addition. `Steps-Mono-Thin.otf` at 32 px (the existing
  HUD-mono face) is the glyph source. We scale via `DrawTextPro`'s
  `fontSize` param down to ~10–18 px on screen. No new TTF download
  needed.
- No editor visualization. Damage numbers don't exist at edit-time.
- No persistent damage log / scoreboard line per hit. The kill feed
  (wan-fixes-13) already covers the "did I get a kill" question;
  damage numbers cover the "how much did THAT hit do" question.
  They're complementary, not redundant.

**Why this fits M6 polish, not a new milestone**

M5 stood up the combat layer with decals + dismemberment + hit-flash
(M5 P12) — every concrete "you got hit" cue except the *numeric*
one. M6 P01 made bones deterministic across the wire so this
feature can spawn from the canonical HIT_EVENT and land at matching
positions on every screen. M6 P02 established the per-event-visual
pattern (jet ignition → particles + SFX). Damage numbers are the
next entry in the same column: a synced visual+audible response to
an already-broadcast event, in service of the **"hits have weight"**
vision pillar.

---

## 1. Behavior in words (the user-facing spec)

Player fires a Pulse Rifle. Bullet lands on a hostile mech's torso.
Inside the same frame, the victim's chest hit-flashes white, blood
sprays, sparks fly, the existing decal lands, and **above the impact
point a small yellow "12" pops into existence**. The 12 has a
slight outward velocity (away from the bullet's path), a small upward
boost, and an initial spin of about half a revolution per second.
Gravity pulls it down. It tumbles. It hits the floor with a faint
metallic *tink* (60% volume) and bounces about a third of its initial
height. It hits again — *tink* — bounces shorter. It comes to rest
on the floor, holds for a quarter-second, then alpha-fades and is
gone. Total elapsed time: ~1.8 seconds.

A Rail Cannon shot lands on the same mech's head for 95 damage. A
**bright red "95"** pops, bigger and bolder than the Pulse Rifle's
12. Same bounce + fade arc. The player understands "I just did big
damage" before they read the digits.

Twelve Microgun rounds land in 200 ms across the same target's torso.
Twelve separate small "8"s spew out at slightly different angles,
each with its own physics, like sparks. They're small enough — and
their motion paths diverge enough — that the screen doesn't drown
in stacked digits.

A frag grenade detonates and hits four mechs at the same tick. Each
of the four mechs gets its own "60"-ish (different per victim
depending on radial falloff). Four numbers spew in four different
directions. The player can read each.

A mech dies from a 150-damage Mass Driver hit. A bright red "150"
pops, the existing kill banner fires above it, the body
ragdolls. The number behaves the same as any other — it doesn't get
a special "killing blow" treatment beyond the color tier its
magnitude already earns. (A `kill_glow` field is reserved in the
flags byte for a future stretch where the killing blow gets a brief
additive halo; v1 doesn't render it.)

The host plays the same scenes back to itself on its own client
window, and remote players see the same numbers above the same mechs
at the same instants (modulo the 100 ms snapshot interp delay, which
applies equally to the visual and the underlying mech motion).

---

## 2. Vision pillar alignment

The damage-number feature is justified by exactly one of the seven
vision pillars (`documents/00-vision.md:24-26`):

> **2. Hits have weight.** A bullet doesn't just decrement a number.
> It rocks the target's skeleton via impulse, leaves a decal,
> splashes blood, makes a *sound* with low end. The visual and
> audible feedback of every shot is a system we build, not a coat
> of polish.

Today, "decrement a number" is precisely what hits do from the
HUD-reader's perspective — the victim's HP bar shrinks by an
unspecified amount. The attacker has no per-hit feedback on the
amount applied; they just see the bar shrink on their next snapshot
of the victim. **Damage numbers close the loop**: the attacker
sees, per shot, the exact tonnage of metal they put through the
target. That's the literal definition of "the visual feedback of
every shot is a system we build, not a coat of polish."

The bounce + tink isn't a coat of polish either — it's the
mechanical-rocket-corpse-physics signature the rest of the game
ships. A floating number that just rises and fades belongs in an
RPG. A number that **falls and tinks** belongs in a game whose
visual identity is metal hitting concrete.

It also aligns with the success criterion ("A new player downloads
the executable, double-clicks it, joins a server, and within sixty
seconds laughs out loud at something that just happened"
— `documents/00-vision.md:71-73`). A 150 spew off a Mass Driver
kill is exactly that kind of moment.

---

## 3. Architecture: one spawn function, two callers, zero new wire bytes

### 3.1 The two-caller pattern (mirrors blood/sparks)

`mech_apply_damage` (src/mech.c) is the **server-side authoritative
damage entry point**. Every damage path funnels through it:
- `weapons_fire_hitscan` (src/weapons.c:380, :494 lag-comp variant)
- `weapons_fire_melee` (src/weapons.c:699, with backstab × 2.5)
- `projectile_step` direct hit (src/projectile.c:454)
- `explosion_spawn` per-victim loop (src/projectile.c:712)
- `mech_apply_environmental_damage` (src/mech.c:1563, DEADLY tiles /
  ACID polys)
- `shotmode kill_peer` (src/shotmode.c:2039, 9999 dmg one-shot)
- our new `shotmode damage` (§7) for the color-tier test

It already spawns 8 blood + 4 sparks server-side on every successful
damage (src/mech.c, post-mech_apply_damage). Then a **second** spawn
fires on every client when the broadcast `NET_MSG_HIT_EVENT` arrives:
`client_handle_hit_event` (src/net.c:2232-2292) spawns its own scaled
blood (`8 + dmg/16` capped at 16) + sparks (`4 + dmg/32` capped at 8).

The reason both spawns coexist without doubling-up on the host UI is
the **wan-fixes-16 thread split**: in multiplayer, the host server
runs on a worker thread with its own `Game` struct + its own
`FxPool`; the host UI is a separate thread / separate `Game` /
separate `FxPool` connecting via UDP loopback. The server-thread's
particle spawns never reach the renderer. Only the UI-thread's
loopback-HIT_EVENT spawn is visible.

In single-process shotmode / offline / `--test-play`, there's only
one `Game`, one `FxPool`. `mech_apply_damage` runs in the same
context as the renderer, so its spawn IS the visible one. There's
no HIT_EVENT broadcast in this mode — no socket bound, no peers — so
no double-spawn.

Damage numbers slot into this exact two-caller shape:

```
src/mech.c::mech_apply_damage (server / single-process path):
    ... existing blood spawn ...
    ... existing spark spawn ...
    fx_spawn_damage_number(&w->fx_pool, hit_pos, hit_dir,
                           (uint8_t)final_dmg_clamped, weapon_id,
                           &w->rng);                          // NEW

src/net.c::client_handle_hit_event (client / loopback path):
    ... existing blood spawn ...
    ... existing spark spawn ...
    fx_spawn_damage_number(&w->fx_pool, decoded_pos, decoded_dir,
                           damage_byte, /*weapon_id=*/0,
                           &w->rng);                          // NEW
```

No coordination, no dedup, no flag check, no extra branch. The same
property that lets blood/sparks coexist lets numbers coexist.

`weapon_id` is passed to the server-side caller because it's
trivially available there (every `mech_apply_damage` caller knows
its weapon). It's reserved for a future "icon next to the number"
treatment — v1 ignores it. The client-side caller passes 0 because
HIT_EVENT doesn't carry weapon_id (it never has — even the kill
feed reads weapon from a separate path). That's fine: the number
itself reads the same on either side; weapon_id is a non-rendered
hint for future use.

### 3.2 Why we don't need a new wire message

HIT_EVENT carries everything:
- `victim_mech_id` (u16) — for log assertions; not needed at render
  time because position is already world-absolute.
- `hit_part` (u8) — also not needed at render time.
- `pos_x_q, pos_y_q` (i16 × 2, ¼-px quantization, ±8190 px range
  per axis) — the spawn position.
- `dir_x_q, dir_y_q` (i16 × 2, Q1.15) — the bullet direction. The
  spawn function flings the number along the perpendicular of dir
  (so it spews up-and-sideways, not back-along-the-bullet-path).
- `damage` (u8, clamped 0–255, **post-armor / post-multiplier final
  value**) — the digits to render and the tier to color.

Total: 12 bytes per hit, unchanged from today. The damage number
adds **zero** to per-tick bandwidth. (Compare M6 P01's `gait_phase_q`
which added 2 bytes/mech/snapshot = ~30 kbps at 32 mechs; this is
~30 kbps cheaper.)

### 3.3 Why server-side cap of u8 is fine for the displayed digits

The hit_event field is `damage_u8` clamped to 255. The biggest
single-hit value in current loadouts is the Mass Driver's center-of-AOE
which can run ~150 with armor + passive modifiers stripped. Pulse
Rifle is 12, Rail Cannon is 80–120, Frag center is ~80. **No legitimate
single-hit damage exceeds 255 in M6.** If a future weapon does
(e.g., a charged Plasma Cannon overcharge), we either keep the cap
(it shows "255" which conveys "huge") or widen the wire field —
that's a P0-scoped wire change for that weapon, not this feature.

---

## 4. Wire format changes

**None.** Existing `NET_MSG_HIT_EVENT` covers it. Protocol id stays
`S0LK` (M6 P02's bump).

---

## 5. New module surface

We don't need a new `.c`/`.h`. The feature folds into `src/particle.{c,h}`
next to the existing `fx_spawn_*` family. This keeps the module count
the same and follows Rule 5 from `documents/01-philosophy.md:103-104`
("A function called from three places earns a non-static life. **The
bar for cross-module function is high.**").

### 5.1 Add to `src/particle.h`

```c
/* Spawn a flying damage number. dir is the bullet/impact unit vector
 * (the perpendicular drives the spew direction); damage_u8 is the
 * post-armor final damage value (0-255); weapon_id is informational
 * (reserved for a future per-weapon glyph treatment, ignored at v1).
 *
 * Spawn site: src/mech.c::mech_apply_damage (server / single-process)
 *           + src/net.c::client_handle_hit_event (multiplayer client).
 * Both call sites are intentional — see documents/m6/08-damage-numbers.md
 * §3.1 for why they don't double-render on the host UI. */
int fx_spawn_damage_number(FxPool *pool, Vec2 pos, Vec2 dir,
                           uint8_t damage_u8, uint8_t weapon_id,
                           WorldRNG *rng);

/* Render pass: convert each FX_DAMAGE_NUMBER particle's world pos to
 * window pixels and DrawTextPro with rotation + outline. Called from
 * renderer_draw_frame AFTER the internal-target upscale blit and
 * BEFORE hud_draw — see §8 for why the placement matters. */
void fx_draw_damage_numbers(const FxPool *pool, const Renderer *r);
```

### 5.2 Add to `src/world.h` (next to existing FxKind enum)

```c
typedef enum {
    FX_BLOOD = 0,
    FX_SPARK,
    FX_TRACER,
    FX_SMOKE,
    FX_STUMP,
    FX_JET_EXHAUST,
    FX_GROUND_DUST,
    FX_DAMAGE_NUMBER,    /* NEW — see documents/m6/08-damage-numbers.md */
    FX_KIND_COUNT
} FxKind;
```

### 5.3 Add to `FxParticle` (in `src/world.h`)

Two new floats, +8 bytes per particle, +64 KB total at the 8000-cap
pool. Negligible vs the 256 MB process budget.

```c
typedef struct {
    Vec2     pos, vel;
    Vec2     render_prev_pos;
    float    life, life_max;
    float    size;
    uint32_t color, color_cool;
    uint8_t  kind, alive;
    int16_t  pin_mech_id;     /* FX_STUMP: parent mech id
                                 FX_DAMAGE_NUMBER: damage value (0-255 today; field
                                                  is i16 so 0-32767 if cap widens) */
    uint8_t  pin_limb;        /* FX_STUMP: limb bit
                                 FX_DAMAGE_NUMBER: bounce counter (0,1,2; ≥2 = at rest) */
    uint8_t  pin_pad;         /* FX_DAMAGE_NUMBER: flags byte
                                 bit 0: tier_is_crit (≥100 dmg)
                                 bit 1: tier_is_heavy (60-99 dmg)
                                 bit 2-7: reserved (future kill_glow, weapon icon, etc.) */
    /* NEW (used only by FX_DAMAGE_NUMBER; other kinds leave them at 0): */
    float    angle;           /* current rotation in radians */
    float    ang_vel;         /* angular velocity in rad/s; bounce damps */
} FxParticle;
```

We **overlay** the FX_STUMP fields rather than adding a separate
union, because:
1. C unions interact awkwardly with our existing zero-init pattern
   (`fx_pool_init` memsets the array to 0).
2. The semantic overlap is harmless: `pin_mech_id` is a 16-bit slot
   that's only meaningful per-kind; FX_STUMP uses it as a mech id,
   FX_DAMAGE_NUMBER uses it as a damage value. No other kind reads
   it. The fx_update branch on `kind` already exists.
3. A comment on the field documents the overlay.

If a future feature needs simultaneous mech-pinning AND a numeric
value on the same particle, we revisit. For damage numbers (which
are never pinned to a mech) the overlay is correct.

### 5.4 Pool capacity

No change. Current `MAX_BLOOD = 8000` (world.h:171). Worst-case
damage-number occupancy is computed in §11; it's ≤ 320 simultaneous
particles, which sits comfortably inside the M6 P02 headroom.

---

## 6. The damage tier table

The single source of truth lives in `src/particle.c` as a const table
indexed by damage tier. The tier is computed once at spawn from the
damage byte; it's stored in the flags byte (`pin_pad`) so the render
pass doesn't recompute.

```c
typedef enum {
    DMG_TIER_LIGHT = 0,    /* 1-9 dmg — pale yellow */
    DMG_TIER_NORMAL,       /* 10-29 dmg — yellow */
    DMG_TIER_MEDIUM,       /* 30-59 dmg — orange */
    DMG_TIER_HEAVY,        /* 60-99 dmg — red-orange */
    DMG_TIER_CRIT,         /* ≥100 dmg — bright red */
    DMG_TIER_COUNT
} DamageTier;

typedef struct {
    uint32_t color;            /* RGBA8 glyph color */
    uint32_t outline;          /* RGBA8 outline color (always near-black, alpha varies by tier) */
    float    font_px;          /* point size for DrawTextPro */
    float    speed_min;        /* initial speed (px/s) lower bound */
    float    speed_max;        /* initial speed upper bound */
    float    upward_bias;      /* extra -Y velocity at spawn (px/s) */
    float    life_s;           /* total life including fade */
} DmgTierDef;

static const DmgTierDef g_dmg_tier[DMG_TIER_COUNT] = {
    /* LIGHT  */ { 0xFFF0B4FF, 0x00000080, 10.0f,  60.0f,  90.0f,  50.0f, 1.4f },
    /* NORMAL */ { 0xFFDC50FF, 0x000000A0, 12.0f,  80.0f, 110.0f,  70.0f, 1.7f },
    /* MEDIUM */ { 0xFF8C28FF, 0x000000C0, 14.0f, 100.0f, 140.0f,  90.0f, 1.9f },
    /* HEAVY  */ { 0xFF5018FF, 0x000000E0, 16.0f, 120.0f, 170.0f, 110.0f, 2.1f },
    /* CRIT   */ { 0xFF2828FF, 0x000000FF, 18.0f, 140.0f, 200.0f, 130.0f, 2.4f },
};

static inline DamageTier dmg_tier_for(uint8_t dmg) {
    if (dmg >= 100) return DMG_TIER_CRIT;
    if (dmg >=  60) return DMG_TIER_HEAVY;
    if (dmg >=  30) return DMG_TIER_MEDIUM;
    if (dmg >=  10) return DMG_TIER_NORMAL;
    return DMG_TIER_LIGHT;
}
```

The threshold cliffs (10 / 30 / 60 / 100) intentionally mirror the
M5 P12 decal cliffs (DENT < 30, SCORCH 30–80, GOUGE ≥ 80 — see
src/mech.c:1940-1942), shifted slightly so the GOUGE/HEAVY split is
clearer to the eye. Mirroring matters: a 75-damage Frag will paint a
SCORCH decal AND show an orange-red "75" — visual consistency across
the two feedback layers, no contradiction.

Color choices are constrained by the **vision aesthetic** (00-vision.md
§Aesthetic): "Blood is bright. Hydraulic fluid, plasma, oil — call it
what you want, but it splashes red and orange". The yellow→red ramp
sits in the same family as blood. We don't use blue/green (would
suggest healing or status; off-vibe) or cyan (already owned by the
crosshair/HUD).

The outline is always near-black with tier-rising alpha — light hits
get a soft 50% outline so they read on bright backgrounds without
hammering them; crits get a fully opaque outline so the number
absolutely punches through any decal/blood/jet plume noise behind it.

The size ramp (10 → 18 px) is small. **Deliberately small** — a
mech's torso renders at ~70 px tall at 1× zoom; an 18 px crit number
is ~1/4 the mech height. Anything bigger would crowd the silhouette.
Anything smaller than 10 px stops being legible. The 10–18 range
gives a clear "bigger means more damage" gradient without ever
swamping the mech.

---

## 7. Per-tick physics step (in `fx_update`)

The existing `fx_update` is a per-kind switch (see particle.c:201-319
research notes). Add a new branch for FX_DAMAGE_NUMBER between the
existing FX_BLOOD branch (which already handles tile collision via
`level_point_solid`) and the FX_GROUND_DUST branch:

```c
case FX_DAMAGE_NUMBER: {
    /* Integrate angular and linear motion. */
    p->ang_vel *= 0.995f;                    /* mild air drag on spin */
    p->angle   += p->ang_vel * dt;
    p->vel.x   *= 0.99f;                     /* mild linear drag */
    p->vel.y   += PARTICLE_GRAVITY * dt;     /* same gravity blood uses */

    Vec2 next = { p->pos.x + p->vel.x * dt,
                  p->pos.y + p->vel.y * dt };

    /* Tile collision — bounce instead of die.
     * Cap at 2 bounces; the third contact rests the particle. */
    uint8_t bounces = p->pin_limb;           /* overlay field */
    if (bounces < 2 && level_point_solid(w->level, next.x, next.y)) {
        /* Crude vertical-bounce model. Sufficient for floors;
         * walls/ceilings: x-component absorbs symmetrically. We don't
         * resolve which axis hit — the level edge geometry of every
         * map keeps damage numbers in the floor regime ~99% of the
         * time. If a number happens to bounce off a ceiling tile, the
         * "tink" still sells it. */
        if (level_point_solid(w->level, next.x, p->pos.y)) {
            p->vel.x = -p->vel.x * 0.55f;
        }
        if (level_point_solid(w->level, p->pos.x, next.y)) {
            p->vel.y = -p->vel.y * 0.55f;
        }
        p->vel.x   *= 0.70f;                 /* friction on horizontal */
        p->ang_vel *= 0.60f;                 /* spin damps on hit */
        p->pin_limb = (uint8_t)(bounces + 1);

        audio_play_at(SFX_DAMAGE_TINK, p->pos);  /* §10 — quiet metallic clink */
    } else if (bounces >= 2 && level_point_solid(w->level, next.x, next.y + 1.0f)) {
        /* Rest contact — let life decay carry it out, no more
         * integration. Pin to ground. */
        p->vel.x = p->vel.y = 0.0f;
        p->ang_vel = 0.0f;
    } else {
        p->pos = next;
    }

    /* Life decay (alpha fade lives in fx_draw_damage_numbers). */
    p->life -= dt;
    if (p->life <= 0.0f) p->alive = 0;
    break;
}
```

Notes:

- We do **not** check polygon collision via the level's poly broadphase.
  Tiles + AABB are sufficient for a glyph that spends < 2 seconds
  airborne. Polygon edges would only matter if the glyph happened to
  spawn inside a sloped alcove; the spawn position comes from a hit
  on a mech (whose pelvis is by definition not inside terrain) so this
  is rare. If it happens, the glyph falls through the slope; visually
  forgivable.
- `PARTICLE_GRAVITY` is the existing tunable used by `FX_BLOOD`. Match
  it so the number falls with the same gravity-feel as the blood it
  spawns alongside.
- The "rest contact" check (`solid below by 1 px`) is the standard
  "am I sitting on the floor?" trick. It costs one extra
  `level_point_solid` call per resting glyph per tick, which is cheap
  (at most ~50 resting glyphs in a heavy fight = ~3000 ops/s, trivial).
- Air drag (0.99) is a soft-landing knob. If glyphs slide too far,
  bump it to 0.97. Tuning in `documents/m5/CURRENT_STATE.md`'s
  tunables table when we ship.

---

## 8. Render pass — sharp world-space text

This is the design's trickiest decision. The M6 P03 capped-internal-RT
architecture means anything drawn inside `BeginMode2D` lands at the
internal resolution (default 1080 lines) and is then bilinear-upscaled
to the window. For thick filled shapes (mechs, plumes, particles) the
upscale is visually correct. **For text glyphs the upscale is fuzzy**
— exactly the reason M6 P03 routes the HUD around the internal RT
("HUD draws at WINDOW resolution on top — sharp text and HUD geometry
regardless of the internal cap" — `src/render.c:1579`).

Damage numbers want world position + sharp pixels. The render path:

```c
renderer_draw_frame(...):
    BeginTextureMode(g_internal_target):
        draw_world_pass(...)      /* world @ internal res */
    EndTextureMode

    /* Halftone shader passes the internal RT into g_post_target. */
    apply_halftone_post(...)

    /* Upscale blit g_post_target → backbuffer with letterbox. */
    DrawTexturePro(g_post_target, ..., blit_scale, blit_dx, blit_dy)

    /* NEW: damage-number pass at window resolution. */
    fx_draw_damage_numbers(&w->fx_pool, renderer);   /* §8.1 */

    /* HUD at window resolution. */
    hud_draw(...)
```

The damage-number pass runs **after** the halftone shader so glyphs
keep their crisp colors — they don't get dithered into the screen-tone
pattern. They sit on top of the halftoned world as a clean overlay,
matching the HUD's visual layer. This also avoids a category of
problem: if the number were drawn inside the world pass, a number
that bounces near a halftone screen edge would get half-dithered
half-clean, which would read as a glitch.

Drawing them after the HUD would be wrong: the HUD covers the
top/bottom of the screen, and a damage number that spawned near a
HUD-occluded part of the world would render OVER the HUD bars,
which is ugly. Pre-HUD is correct.

### 8.1 The render pass body

```c
void fx_draw_damage_numbers(const FxPool *pool, const Renderer *r)
{
    if (pool->count == 0) return;
    Font font = g_ui_font_mono;  /* Steps-Mono-Thin 32px source size */

    for (int i = 0; i < pool->count; ++i) {
        const FxParticle *p = &pool->items[i];
        if (!p->alive || p->kind != FX_DAMAGE_NUMBER) continue;

        /* World → window via the same blit numbers M6 P03 uses for the
         * HUD's off-screen-indicator transform. */
        Vec2 internal_xy = renderer_world_to_internal(r, p->pos);
        Vector2 screen_xy = {
            internal_xy.x * r->blit_scale + r->blit_dx,
            internal_xy.y * r->blit_scale + r->blit_dy
        };

        DamageTier tier = (p->pin_pad & 1) ? DMG_TIER_CRIT
                       : (p->pin_pad & 2) ? DMG_TIER_HEAVY
                       : dmg_tier_for((uint8_t)p->pin_mech_id);  /* fallback decode */
        const DmgTierDef *def = &g_dmg_tier[tier];

        /* Build the digit string. damage_value is in pin_mech_id (int16, ≤255 today). */
        char buf[8];
        int n = (int)p->pin_mech_id;
        snprintf(buf, sizeof buf, "%d", n);

        /* Alpha fade in the last 0.5s of life. */
        float life_t = p->life / p->life_max;
        float alpha = 1.0f;
        if (p->life < 0.5f) alpha = p->life / 0.5f;
        if (alpha < 0.0f) alpha = 0.0f;

        Color col   = color_rgba8_apply_alpha(def->color,  alpha);
        Color outln = color_rgba8_apply_alpha(def->outline, alpha);
        float deg   = p->angle * (180.0f / PI);

        /* Render-pixel font size = tier base × dpi_scale × bounce_squash.
         * bounce_squash makes the number squish vertically by 5% on
         * each ground contact — sells the impact without bloating the
         * physics code. */
        float bounce_squash = 1.0f - 0.05f * (float)p->pin_limb;
        float fs = def->font_px * r->dpi_scale * bounce_squash;

        Vector2 origin = { 0 };   /* rotate around top-left for simplicity */

        /* 4-pass outline: render the string 4 times at +/- 1 px offset
         * in cardinal directions, then once at center in glyph color.
         * Cheaper than a real outline shader, visually equivalent at
         * small sizes. */
        for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) {
            if (dx == 0 && dy == 0) continue;
            if (dx != 0 && dy != 0) continue;   /* skip diagonals — 4 passes total */
            Vector2 off = { screen_xy.x + (float)dx, screen_xy.y + (float)dy };
            DrawTextPro(font, buf, off, origin, deg, fs, 1.0f, outln);
        }
        DrawTextPro(font, buf, screen_xy, origin, deg, fs, 1.0f, col);
    }
}
```

### 8.2 The `renderer_world_to_internal` helper

`src/render.c` already exposes `renderer_screen_to_world` (the inverse,
used for cursor → world). We add the forward direction next to it:

```c
Vec2 renderer_world_to_internal(const Renderer *r, Vec2 world)
{
    /* Same math as raylib's GetWorldToScreen2D but using r->internal_cam
     * (the camera computed for the internal RT this frame). */
    Vector2 v = GetWorldToScreen2D((Vector2){ world.x, world.y },
                                   r->internal_cam);
    return (Vec2){ v.x, v.y };
}
```

This is genuinely small. If we discover we need it from other render
passes (HUD off-screen indicators already roll their own; particles
have their own pos→screen path; not many callers exist) we leave it
in render.c. If only damage numbers ever call it, we leave it in
render.c anyway — the function is too small to warrant its own file.

---

## 9. Spawn function — what flies out where

```c
int fx_spawn_damage_number(FxPool *pool, Vec2 pos, Vec2 dir,
                           uint8_t damage_u8, uint8_t weapon_id,
                           WorldRNG *rng)
{
    if (damage_u8 == 0) return -1;   /* don't render zero-damage events (armor absorbed) */

    int slot = fx_pool_acquire(pool);
    if (slot < 0) return -1;
    FxParticle *p = &pool->items[slot];

    DamageTier tier = dmg_tier_for(damage_u8);
    const DmgTierDef *def = &g_dmg_tier[tier];

    /* Spew direction: perpendicular to dir + an upward bias.
     * dir points along the bullet path (toward the victim). The
     * perpendicular is (-dir.y, +dir.x); we flip its sign randomly so
     * sequential hits spew alternate sides instead of all stacking.
     * Add a small cone jitter so two identical hits don't stack visually. */
    float perp_sign = (world_rng_next_u32(rng) & 1u) ? 1.0f : -1.0f;
    Vec2 perp = { -dir.y * perp_sign, dir.x * perp_sign };
    float cone = world_rng_next_f01(rng) * 0.6f - 0.3f;   /* ±0.3 rad */
    float c = cosf(cone), s = sinf(cone);
    Vec2 spew = { perp.x * c - perp.y * s, perp.x * s + perp.y * c };

    float speed = def->speed_min +
                  (def->speed_max - def->speed_min) * world_rng_next_f01(rng);

    p->pos = (Vec2){ pos.x, pos.y - 8.0f };   /* small upward offset so spawn isn't inside the limb */
    p->vel = (Vec2){ spew.x * speed,
                     spew.y * speed - def->upward_bias };
    p->render_prev_pos = p->pos;
    p->life     = def->life_s;
    p->life_max = def->life_s;
    p->size     = def->font_px;       /* not used for circle draw; informational */
    p->color    = def->color;
    p->color_cool = def->outline;
    p->kind     = FX_DAMAGE_NUMBER;
    p->alive    = 1;
    p->pin_mech_id = (int16_t)damage_u8;   /* digits to render */
    p->pin_limb    = 0;                    /* bounce counter */
    p->pin_pad     = (uint8_t)((tier == DMG_TIER_CRIT  ? 0x1 : 0u) |
                               (tier == DMG_TIER_HEAVY ? 0x2 : 0u));
    p->angle    = (world_rng_next_f01(rng) * 0.4f) - 0.2f;     /* tiny initial tilt */
    p->ang_vel  = (world_rng_next_f01(rng) * 6.0f) - 3.0f;      /* ±3 rad/s spin */

    (void)weapon_id;   /* reserved — v1 ignores */
    return slot;
}
```

The perp_sign-flip + cone jitter is the **anti-stacking** behavior
called out in the user spec ("Make sure the numbers are small the
damage numbers should not overwhelm the size of the mech"). Twelve
Microgun rounds in 200 ms produce twelve spews, each at a slightly
different angle — they fan out into a visible spray instead of stacking
into an unreadable column. The cone is intentionally small (±0.3 rad
≈ ±17°) so the spew direction still **reads as "away from the bullet
direction"** rather than as random sparks.

---

## 10. Audio — tink on bounce

### 10.1 Asset

One new SFX entry `SFX_DAMAGE_TINK` in the `SfxId` enum
(src/audio.h:31-119, near `SFX_HIT_METAL`). Sourced from
`kenney_impact-sounds/Audio/impactMetal_light_000.ogg` (per the
already-vendored asset pack referenced in
`tools/audio_inventory/source_map.sh` line 132). Pipeline:

1. New line in `tools/audio_inventory/source_map.sh`:
   ```sh
   mkwav "$K/kenney_impact-sounds/Audio/impactMetal_light_000.ogg" \
         "$SFX/damage_tink.wav" 0.18
   ```
   The 0.18 s duration cap keeps it under the "feels like a tink not
   a thud" threshold. Two more alias files (`impactMetal_light_001/002.ogg`)
   are pulled in the same way for natural variation (audio.c manifest
   already supports per-SFX alias arrays — see SFX_FOOTSTEP_METAL for
   the 5-alias pattern).
2. `make audio-inventory` materializes the WAVs from the source pack.
3. `make audio-normalize` runs `tools/audio_normalize/normalize.sh`
   to convert to 22050 Hz mono PCM16.
4. New entry in `audio.c`'s `g_sfx_manifest` (src/audio.c:254-346)
   listing the WAV + aliases + bus + base volume.

**v1 fallback path**: if the audio asset PR hasn't landed when this
feature ships, the bounce code path uses the existing
`SFX_HIT_METAL` (already loaded, audio.h-defined, audible). The
fallback is a one-line `#ifndef SFX_DAMAGE_TINK` toggle in
`particle.c`. The audio is the second-most-noticeable part of the
feature (after the color); shipping with the placeholder is
acceptable for the first review pass.

### 10.2 Volume

Bounce SFX is **incidental**, not central — the player just shot
something; the central audio cue is the weapon fire + the existing
`SFX_HIT_METAL` / `SFX_HIT_FLESH` impact sound (which already plays
on the original projectile collision). The damage-number bounce is
the **secondary** "and now the number bounces around on the floor"
beat. Set `audio_play_at` base volume to **0.30**, well below the
0.85 of `SFX_HIT_METAL`. In a 16-mech firefight with 20 simultaneous
bounces, that volume keeps the bounces present-but-not-overwhelming.

This honors the memory entry "Shot-test audio default should be quiet
(≤30%)" — same constraint applies to ambient bounce SFX during real
play.

### 10.3 Spatialization

Reuses `audio_play_at(SfxId, Vec2 pos)` (src/audio.c:587-617).
- Listener: local mech's chest (default).
- 200 px → 1500 px linear falloff.
- ±800 px pan.

A bounce on the other side of the map plays inaudibly; a bounce near
the player is sharply audible from the bounce location. Same as every
other positional SFX in the game.

### 10.4 Tier-keyed variant? (deferred)

We *could* swap to a heavier "thunk" SFX for HEAVY/CRIT tier bounces
(parallel to `SFX_JET_IGNITION_CONCRETE` vs `SFX_JET_IGNITION_ICE`'s
material keying). For v1 we don't: one tink covers all tiers, keeps
the asset count down, keeps the test surface small. If playtest
feedback says crit bounces should feel heavier, add `SFX_DAMAGE_THUNK`
later — three-line change. The framework's already there.

---

## 11. Bandwidth + CPU + memory budget

| Resource | Cost | vs budget |
|---|---|---|
| Wire bytes added | **0** | reuses HIT_EVENT |
| FxParticle bloat | +8 B × 8000 = +64 KB | inside 256 MB process budget (0.025%) |
| New SFX asset on disk | ~30 KB (3 aliases × ~10 KB) | inside assets/sfx budget |
| Worst-case simultaneous numbers | 16 mechs × ~10 dmg-events/s × 2.0 s avg life ≈ **320** | inside FxPool's ~320 headroom after M6 P02 jet budget |
| Per-tick CPU | 320 particles × (gravity int + tile-solid query + optional bounce branch + rot) ≈ ~3000 ops | trivial vs 60 Hz physics step |
| Per-frame render | 320 particles × (world→screen + 5 DrawTextPro) ≈ ~1600 text draws | well inside frame budget; raylib's DrawTextPro is GPU-batched per font texture |

The 320-particle worst-case math:

> 16 active mechs × ~10 damage events per second per mech in heavy
> combat (a Microgun on continuous fire is ~10 Hz; an AOE burst is
> ~4 victims × 1 event = 4 events at once; averaging ~10/s/mech is
> generous) × 2.0 s NORMAL-tier average life = 320 simultaneous
> particles.

For the FxPool: M6 P02 budget worst-case was ~7680 live jet
particles (16 mechs × 2 nozzles × 8 particles/tick × 60 Hz × 0.5 s).
That leaves ~320 headroom at the 8000 cap. Damage numbers worst-case
matches exactly. **The pool cap does not need to grow if you also
expect jet particles at saturation simultaneously**; if both occur,
the FX_BLOOD/SPARK spawns from the firefight could be silently
dropped on the overflow path (the existing `fx_pool_acquire` returns
-1 when full and callers silently skip). For safety we bump
`MAX_BLOOD` from 8000 to **8500** — a 32 KB increase, trivial, and
keeps a clean 200-slot reserve above the worst combined case.

(Yes, the constant is misnamed — it's the FxPool cap, not the blood
cap. Renaming `MAX_BLOOD` → `FX_POOL_CAP` is a 5-call rename and
properly belongs in this PR. See §15 files-to-touch.)

---

## 12. Implementation phases

Each phase ends with the build green and existing tests passing.
Land them in this order — earlier phases don't require later phases.

### Phase 1 — Audio asset + new SFX id

- Add `SFX_DAMAGE_TINK` to `src/audio.h` (next to `SFX_HIT_METAL`).
- Add 3 alias entries to `src/audio.c::g_sfx_manifest` (mirroring
  `SFX_FOOTSTEP_METAL`'s pattern at lines 254-346).
- Add `mkwav` line(s) to `tools/audio_inventory/source_map.sh`.
- Run `make audio-inventory && make audio-normalize` to materialize
  + normalize the WAVs.
- Update `tools/audio_credits/` if it auto-generates from
  source_map.sh (per CLAUDE.md, `make audio-credits` is wired).
- Smoke test: `make test-audio-smoke` — verify the new SFX loads.

Phase 1 is fully independent of the rest. If the asset pull stalls,
the rest proceeds against the v1 fallback (`SFX_HIT_METAL`).

### Phase 2 — FxPool extension

- Add `FX_DAMAGE_NUMBER` to `FxKind` (src/world.h:648-657).
- Add `float angle, ang_vel;` to `FxParticle` (src/world.h:659-683).
- Bump `MAX_BLOOD` 8000 → 8500 and (optionally) rename to
  `FX_POOL_CAP` across the 5 callsites.
- No behavior change yet — every existing kind still has angle=0,
  ang_vel=0 (zero-init).
- Run `make` + `make test-physics` + `tests/net/run.sh` to confirm
  nothing regressed.

### Phase 3 — Spawn function + damage tier table

- Add `DamageTier` enum + `DmgTierDef` struct + `g_dmg_tier[]` table
  to `src/particle.c`.
- Add `dmg_tier_for(uint8_t)` static inline.
- Implement `fx_spawn_damage_number(...)` in `src/particle.c`,
  declare in `src/particle.h`.
- Add a `SHOT_LOG` line in the spawn body:
  ```c
  SHOT_LOG("dmgnum spawn pos=%.1f,%.1f dir=%.2f,%.2f dmg=%u tier=%d",
           pos.x, pos.y, dir.x, dir.y, damage_u8, (int)tier);
  ```
  This is the assertion target for the paired-shot test in §13.
- No callers yet. Build green, existing tests green.

### Phase 4 — fx_update physics branch

- Add the `case FX_DAMAGE_NUMBER:` branch in
  `src/particle.c::fx_update`.
- Use existing `level_point_solid` for collision (the same call
  blood already makes).
- Audio: emit `audio_play_at(SFX_DAMAGE_TINK, p->pos)` on bounce,
  with a `#ifndef SFX_DAMAGE_TINK_AVAILABLE` fallback to
  `SFX_HIT_METAL` (or just unconditionally use SFX_HIT_METAL if
  Phase 1 hasn't landed).
- Build green. Spawn manually via a one-off test (or wait for §13's
  test to drive it).

### Phase 5 — Render pass

- Add `renderer_world_to_internal` helper in `src/render.c` (next
  to `renderer_screen_to_world`).
- Implement `fx_draw_damage_numbers(pool, renderer)` in
  `src/particle.c` (it needs `Font g_ui_font_mono` from platform.h
  and `DrawTextPro` from raylib — both already in scope).
- Wire one call in `renderer_draw_frame` after the upscale blit and
  before `hud_draw` (src/render.c:1567-1587 area).
- Visually verify with a single-process shotmode run (`Phase 6`).

### Phase 6 — Single-process color-tier test

- Add the `damage <mech_id> <part> <amount>` shotmode directive to
  `src/shotmode.c` (next to `kill_peer` at ~line 539). Parser +
  per-tick handler that calls `mech_apply_damage` with the given args.
- Write `tests/shots/m6_damage_number_tiers.shot`:
  - `map slipstream`
  - `spawn_at <wx> <wy>`
  - `extra_chassis dummy <wx+200> <wy>` (a stationary target)
  - `at 30 damage 1 PART_CHEST 5`   (LIGHT)
  - `at 35 damage 1 PART_CHEST 20`  (NORMAL)
  - `at 40 damage 1 PART_CHEST 45`  (MEDIUM)
  - `at 45 damage 1 PART_CHEST 75`  (HEAVY)
  - `at 50 damage 1 PART_CHEST 150` (CRIT)
  - `at 60 shot tier_spawn`
  - `at 120 shot tier_midflight`
  - `at 180 shot tier_settled`
  - `at 200 end`
  - `contact_sheet tier_overview cols 3 cell 480 270`
- Manual visual review on the contact sheet — verify color ramp
  matches §6's table.

### Phase 7 — Hook into the two damage paths

- In `src/mech.c::mech_apply_damage`, after the existing blood+spark
  spawn block, add:
  ```c
  uint8_t dmg_u8 = (final_dmg > 255.0f) ? 255 : (uint8_t)final_dmg;
  if (dmg_u8 > 0) {
      fx_spawn_damage_number(&w->fx_pool, hit_pos, hit_dir,
                             dmg_u8, weapon_id_for_this_path,
                             &w->rng);
  }
  ```
  `weapon_id_for_this_path` is whatever the calling weapon path
  has in scope (each caller — weapons.c, projectile.c, explosion AOE,
  environmental — has its own weapon id or 0 for environmental).
- In `src/net.c::client_handle_hit_event`, after the existing blood+
  spark spawn block (~net.c:2254), add the equivalent call with the
  decoded `damage_byte` (and `weapon_id = 0` since HIT_EVENT doesn't
  carry it):
  ```c
  if (damage_byte > 0) {
      fx_spawn_damage_number(&w->fx_pool, hit_pos, hit_dir,
                             damage_byte, /*weapon_id=*/0,
                             &w->rng);
  }
  ```

### Phase 8 — Paired-process sync test

- Write `tests/shots/net/2p_damage_numbers.host.shot`:
  - `network host 24091`
  - `spawn_at 2300 1112`
  - `peer_spawn 1 2500 1112`
  - `loadout TROOPER PULSE_RIFLE SIDEARM ARMOR_LIGHT JET_STANDARD`
  - `aim 2500 1120`
  - `at 30 tap fire`     (one Pulse Rifle hit, deterministic 12 dmg)
  - `at 40 tap fire`
  - `at 50 tap fire`
  - `at 200 shot host_after_3hits`
  - `at 240 end`
- Mirror for `.client.shot` (connect to 127.0.0.1:24091, matching
  spawn_at 2500 1112, passive aim).
- Write `tests/shots/net/run_damage_numbers.sh` (model on
  `run_riot_cannon_sfx.sh`):
  - Run paired processes, collect logs.
  - `asrt "host spawned 3 numbers"  "[ \"\$(grep -c 'dmgnum spawn' \"\$HOST_LOG\")\" = 3 ]"`
  - `asrt "client spawned 3 numbers" "[ \"\$(grep -c 'dmgnum spawn' \"\$CLIENT_LOG\")\" = 3 ]"`
  - `asrt "host first dmg = 12" "grep -qE 'dmgnum spawn .* dmg=12 ' \"\$HOST_LOG\""`
  - `asrt "client first dmg = 12" "grep -qE 'dmgnum spawn .* dmg=12 ' \"\$CLIENT_LOG\""`
  - `asrt "tier matches NORMAL=1 on both" \
        "diff <(grep 'dmgnum spawn' \"\$HOST_LOG\" | awk '{print \$NF}') \
              <(grep 'dmgnum spawn' \"\$CLIENT_LOG\" | awk '{print \$NF}')"`
- Add `test-damage-numbers` to the Makefile.

### Phase 9 — Update existing baseline tests, then docs

- Run the full baseline: `make test-physics test-pickups test-ctf
  test-snapshot test-spawn test-prefs test-map-chunks
  test-map-registry test-frag-grenade test-grapple-ceiling`.
- Run paired baselines: `tests/net/run.sh`, `tests/net/run_3p.sh`,
  all `tests/shots/net/run_*.sh`. None should fail; damage numbers
  are an additive visual that doesn't change game state.
- Update `documents/m5/CURRENT_STATE.md` tunables table with the
  `g_dmg_tier` entries.
- Update `CLAUDE.md` status line to mention M6 P04 (or whatever P-
  number this lands at — currently this would be **M6 P04** since
  P03 was the capped internal RT; numbering at land-time).
- Append the Phase status footnote at the end of this document.

---

## 13. Test plan (full detail)

### 13.1 Single-process color-tier test

`tests/shots/m6_damage_number_tiers.shot`. Spawns the local mech and
one stationary `extra_chassis` dummy 200 px to the right. Uses the
new `damage` shotmode directive to apply exactly one hit of each
of the five tiers, captured at three points along each number's life:
- t = 60 — spawn flash, fresh spew
- t = 120 — midflight, post-first-bounce
- t = 180 — settled, mid-fade

Output: 15 shot PNGs composited into a `contact_sheet` PNG. Manual
verification: the 5 colors must read distinctly in the spawn frame;
the spew arcs must be visibly different magnitudes (NORMAL spews
less far than CRIT); all 5 numbers should be airborne in the midflight
frame and on/near the floor in the settled frame.

The test is **manual visual review** for v1 — the SHOT_LOG line
covers spawn correctness; the contact sheet covers visual correctness.
If a future asserter wants to pixel-diff the contact sheet against
a golden, that's a separable enhancement (the M5 P10 chassis-distinctness
shot uses the same pattern).

### 13.2 Paired-process sync test

`tests/shots/net/run_damage_numbers.sh` per Phase 8 above. The
test exists to prove the **wire round-trip**: a damage event
generated on the host produces an identical damage-number spawn on
the client. The 5 assertions in §12 Phase 8 cover:
1. Host emitted 3 spawn log lines (one per fire).
2. Client emitted 3 spawn log lines (received 3 HIT_EVENTs from host).
3. Host's first dmg is 12 (Pulse Rifle baseline).
4. Client's first dmg is 12 (matches host).
5. Tier columns from both logs diff cleanly (every spawn agrees on
   tier).

If the loadout sync regresses (per the M5 P17 sync-fix in CLAUDE.md),
this test catches it because mismatched loadouts produce mismatched
damage values.

### 13.3 Existing tests must stay green

The damage-number spawn is purely additive — it doesn't change
gameplay state, doesn't broadcast new wire events, doesn't alter
HP / armor / kill logic. Every existing paired test should pass
unchanged:
- `tests/net/run.sh` — 13/13 baseline.
- `tests/net/run_3p.sh` — 10/10.
- `tests/shots/net/2p_dismember.{host,client}.shot` — verifies the
  M5 P12 dismember + decal layer still works; should still pass and
  will now ALSO display the dismember-blow damage number.
- `tests/shots/net/run_kill_feed.sh` — kill feed unaffected (kill
  feed is its own wire event, NET_MSG_KILL_EVENT).
- `tests/shots/net/run_riot_cannon_sfx.sh` — predict/sfx gate
  unaffected.

### 13.4 Visual regression: shotmode `m5_drift_isolate.shot`

This test stands a Trooper still for 200 ticks and verifies no pelvis
drift. The pose-compute test from M6 P01 is the canonical anti-drift
guard. Damage numbers shouldn't fire at all on this test (no damage
events). Verify no `dmgnum spawn` lines appear in the log.

### 13.5 Stress test (manual)

Run a 4-bot match on Concourse with Microguns + Frag Grenades enabled.
Watch for:
- Pool overflow (no `dmgnum spawn pool_full` warnings).
- Visual clutter (numbers should fan out, not stack).
- Frame rate (no perceptible drop in the `--perf-overlay` FPS readout
  when 50+ numbers are airborne simultaneously).

This is the "did we get the feel right" check that no automated test
covers. The trade-off in §15 calls out that visual clutter is a
known concern; the stress test is how we measure it before shipping.

---

## 14. Sequence-of-events diagram

```
Tick T (host server thread):
    weapons_fire_hitscan(shooter=A, target=B, weapon=PULSE_RIFLE)
        ├─ ray hits B's chest
        └─ mech_apply_damage(B, PART_CHEST, 12.0, dir, A)
            ├─ B.health -= 12
            ├─ fx_spawn_blood × 8  (into SERVER-THREAD FxPool)
            ├─ fx_spawn_spark × 4  (into SERVER-THREAD FxPool)
            ├─ fx_spawn_damage_number(pos, dir, 12, PULSE_RIFLE)  ◄ NEW
            │                          (into SERVER-THREAD FxPool — invisible in multiplayer,
            │                           visible in single-process shotmode)
            └─ queue HitFeedEntry → net_server_broadcast_hit
                └─ NET_MSG_HIT_EVENT serialized + sent over UDP to all peers
                                      including the host's own UI client (loopback)

Tick T+0..1 (host UI client thread, ~1-frame latency over loopback):
    client_handle_hit_event(victim=B, part=CHEST, pos_q, dir_q, dmg=12)
        ├─ fx_spawn_blood × (8 + 12/16)  (into UI-THREAD FxPool — visible)
        ├─ fx_spawn_spark × (4 + 12/32)  (into UI-THREAD FxPool — visible)
        ├─ fx_spawn_damage_number(pos, dir, 12, /*weapon=*/0)  ◄ NEW (visible)
        └─ B.hit_flash_timer = 0.10s

Tick T+0..1 (remote client X thread):
    same handler runs, same FxPool, same spawn.

Tick T+0..N (every client tick):
    fx_update advances each FX_DAMAGE_NUMBER particle:
        - integrates gravity + drag + angular vel
        - on tile contact: bounce + audio_play_at(SFX_DAMAGE_TINK)
        - on life ≤ 0: alive = 0

Tick T+0..N (every render frame):
    renderer_draw_frame:
        ├─ world pass into internal RT (mechs, plumes, blood, sparks, ...)
        ├─ halftone shader pass
        ├─ upscale blit to backbuffer
        ├─ fx_draw_damage_numbers  ◄ NEW (sharp, at window resolution)
        └─ hud_draw                     (existing)
```

---

## 15. Risks and trade-offs

### Risks

- **Visual clutter at saturation.** A 16-mech firefight with Microguns
  could spew 50+ numbers per second across the screen. The anti-stack
  cone-jitter at spawn helps, but high-DPS-low-per-hit weapons are
  the worst case. **Mitigation**: small font sizes (10–18 px), short
  lifetimes (1.4–2.4 s), color-tier gradient so the eye triages
  "small yellow = ignore, big red = matters". If playtest signals
  more clutter than this handles, the v2 path is **per-source damage
  combining**: if mech A hits mech B three times within 200 ms with
  the same weapon, combine into one "+36" number instead of three "+12"s.
  Combining is a separate PR; see deferred items.

- **Sync drift on bounce.** Two clients receive HIT_EVENT at slightly
  different ticks (UDP latency). Each spawns the number with its own
  `WorldRNG` state. Cone-jitter and bounce trajectories will diverge.
  **Mitigation**: don't sweat it. The number is a visual; ±20 px
  divergence between two players' screens for the same hit is
  imperceptible. We don't aim for bit-exact cross-client visuals
  (M6 P01 only aims for ≤1 px on **bones**, not on free particles).
  The damage VALUE always agrees because both clients decode the
  same wire byte. The numeric agreement is what matters; the trajectory
  is decorative.

- **Number bounces off a wall mid-flight and lands inside terrain.**
  The crude 1-axis bounce code in §7 doesn't handle 45° wall hits
  cleanly. The number can end up sliding into a slope and falling
  through the world. **Mitigation**: capped life (≤ 2.4 s) ensures
  it disappears whether it found a resting place or not. The
  `pin_limb` bounce-counter caps re-bounces at 2 even when the
  geometry would allow more, so the number stops being CPU work
  after 2 contacts regardless.

- **Audio spam.** 50 bounces in a second at 30% volume is still 50
  audio plays — the audio mixer might miss them if voice budget is
  exceeded. **Mitigation**: the existing audio_play_at silently
  drops voices over budget (raylib's `LoadSound` voice pool has
  a fixed cap). The drop is graceful — no crash, just no
  audible tink for that one bounce. Acceptable for an incidental SFX.

- **Internationalization.** Right now we render ASCII digits 0-9.
  No I18N concern at v1; if a future build localizes the game, the
  number itself is locale-neutral (we don't print "12 damage" — just
  "12"). Comma-vs-period decimal grouping doesn't matter at sub-256
  values. Cross-script considerations are nil.

### Pre-disclosed trade-offs we expect to log

Add these to `TRADE_OFFS.md` when the work lands:

- **"Damage numbers spawn in two places (server-side
  `mech_apply_damage` + client-side `client_handle_hit_event`),
  same pattern as blood/sparks."** The duplication is intentional:
  in multiplayer the host server thread's spawn is invisible, only
  the loopback HIT_EVENT path renders on the host UI. In single-
  process / offline, only the server-side spawn renders (no HIT_EVENT).
  Anyone reading either site might wonder why both calls exist; the
  trade-off entry documents it.

- **"Damage number trajectories don't cross-client sync."** The
  number VALUES are wire-deterministic; the SPEW DIRECTIONS use each
  client's local RNG and drift apart. Acceptable per §15 risks.

### Trade-offs this work CREATES (deletion triggers)

None — it's an additive feature; nothing it touches has a
"deprecate when X ships" entry.

### Trade-offs this work RETIRES (deletion targets)

None outright, but it **partially closes** the gap on
documents/00-vision.md's "Hits have weight" pillar in a way that
no prior milestone has. CURRENT_STATE.md should note the closure.

---

## 16. Files to touch (cheat sheet)

| File | Change |
|---|---|
| `src/world.h` | Add `FX_DAMAGE_NUMBER` to `FxKind`; add `float angle, ang_vel` to `FxParticle`; bump `MAX_BLOOD` 8000 → 8500 (or rename → `FX_POOL_CAP`) |
| `src/particle.h` | Add `fx_spawn_damage_number` + `fx_draw_damage_numbers` prototypes |
| `src/particle.c` | `DamageTier` enum + `g_dmg_tier[]` table + `dmg_tier_for()` + spawn impl + `case FX_DAMAGE_NUMBER:` in `fx_update` + `fx_draw_damage_numbers` body + SHOT_LOG line |
| `src/mech.c` | Call `fx_spawn_damage_number` after blood/sparks in `mech_apply_damage` |
| `src/net.c` | Call `fx_spawn_damage_number` after blood/sparks in `client_handle_hit_event` |
| `src/render.c` | `renderer_world_to_internal` helper; one `fx_draw_damage_numbers` call between upscale-blit and `hud_draw` |
| `src/audio.h` | New `SFX_DAMAGE_TINK` enum entry |
| `src/audio.c` | New manifest entry in `g_sfx_manifest` (with aliases) |
| `tools/audio_inventory/source_map.sh` | New `mkwav` lines for damage_tink + aliases |
| `src/shotmode.c` | New `damage <mech_id> <part> <amount>` directive |
| `tests/shots/m6_damage_number_tiers.shot` | NEW — 5-tier visual contact sheet |
| `tests/shots/net/2p_damage_numbers.host.shot` | NEW |
| `tests/shots/net/2p_damage_numbers.client.shot` | NEW |
| `tests/shots/net/run_damage_numbers.sh` | NEW — paired-sync assertions |
| `Makefile` | Wire `test-damage-numbers` target |
| `CLAUDE.md` | Bump status line to mention M6 P04 (or P05 — at land-time numbering) |
| `documents/m5/CURRENT_STATE.md` | Add `g_dmg_tier` table to tunables list; note feature in "Recently added" |
| `TRADE_OFFS.md` | Add two entries per §15 |
| `documents/m6/08-damage-numbers.md` | THIS FILE — append status footnote when each phase lands |

---

## 17. References

External (research):
- [Damage Numbers in RPGs — Shweep / Medium](https://shweep.medium.com/damage-numbers-in-rpgs-1f0e3b1bc23a) — case for "easy readability regardless of style" + bounce-naturally-adds-pop-flavor
- [Floating Point Combat Systems — Game Developer](https://www.gamedeveloper.com/design/floating-point-combat-systems) — classic / arc / bounce animation taxonomy
- [MMFloatingText — Feel Documentation](https://feel-docs.moremountains.com/mm-floating-text.html) — production reference for the bounce-gravity variant we use
- [Damage Number Philosophy — RPGMaker](https://rpgmaker.net/articles/2754/) — color-tier and font-monospace readability arguments
- [Deep Rock Galactic discussion — Steam Community](https://steamcommunity.com/app/548430/discussions/3/3183487594851122502/) — counter-evidence that visible damage feedback can be visual clutter at saturation; informs our §15 risk + the deferred per-source-combining future path

Internal (this codebase):
- `documents/00-vision.md:24-26` — "Hits have weight" pillar (THE justification for this feature)
- `documents/00-vision.md:44-49` — Aesthetic constraints driving color palette (yellow/red/orange family, not neon)
- `documents/01-philosophy.md:13-17` — "less code, of higher quality" — the no-new-module decision
- `documents/01-philosophy.md:103-104` — Rule 5: function-from-three-places earns a non-static life; spawn is called from 2 places, so the API gets a third (the render) and goes through `particle.h`
- `src/world.h:171` — `MAX_BLOOD = 8000` (FxPool cap; bumping to 8500 in Phase 2)
- `src/world.h:648-657` — FxKind enum (where to add)
- `src/world.h:659-683` — FxParticle struct (where to add fields)
- `src/particle.c:201-319` — fx_update per-kind switch (where to add the new branch)
- `src/particle.c:356-447` — fx_draw two-pass batched render (separate from our new pass; see §8)
- `src/mech.c:159-160` — `mech_apply_damage` signature
- `src/mech.c:1940-1942` — M5 P12 decal kind thresholds (mirrored by our tier thresholds)
- `src/net.c:2232-2292` — `client_handle_hit_event` (second spawn site)
- `src/net.c:3118-3159` — NET_MSG_HIT_EVENT wire format (12 bytes, everything we need)
- `src/render.c:1387-1640` — renderer_draw_frame (where the new render pass slots in)
- `src/render.c:1567-1587` — upscale blit + HUD area (the pre-HUD insertion point)
- `src/render.c:1579-1581` — comment establishing "HUD draws at WINDOW resolution on top — sharp text"; we follow the same pattern
- `src/platform.c:14-17` — `g_ui_font_mono` declaration
- `src/platform.c:136-142` — Steps-Mono-Thin.otf @ 32 px (the font we render with)
- `src/audio.c:587-617` — audio_play_at spatialization (listener / falloff / pan)
- `src/audio.c:254-346` — g_sfx_manifest (where SFX_DAMAGE_TINK entry goes)
- `src/audio.h:31-119` — SfxId enum
- `tools/audio_inventory/source_map.sh:132` — example mkwav line (template)
- `tools/audio_normalize/normalize.sh:49,56` — target format constants (22050 Hz mono PCM16)
- `documents/m6/02-jetpack-propulsion-fx.md` — the per-event-visual pattern we mirror (sim → FxPool → render)
- `documents/m6/01-ik-and-pose-sync.md` — the deterministic-sync pattern; we benefit from the per-tick HIT_EVENT broadcast being canonical
- `documents/m6/03-perf-4k-enhancements.md` — the capped internal RT architecture; we draw OUTSIDE the RT for sharp text (§8)
- `CURRENT_STATE.md` — "wan-fixes-16" thread split context (why server-side spawn doesn't double-render on host UI)
- `CLAUDE.md` — M5 P12 hit_flash + per-limb decal context (what damage numbers complement)
- `TRADE_OFFS.md` — convention for tracking new entries (we add 2 per §15)

---

## 18. Definition of done

The work is done when:

1. `make` builds clean with `-Wall -Wextra -Wpedantic -Werror`.
2. `make test-audio-smoke` passes (new SFX entry loads or fallback
   path is wired without errors).
3. `make test-physics` passes (no regression in particle integration).
4. All paired baselines pass: `tests/net/run.sh` (13/13),
   `tests/net/run_3p.sh` (10/10), every existing `tests/shots/net/run_*.sh`.
5. `make test-damage-numbers` passes (paired host/client damage
   numbers sync test from §13.2, 5 assertions).
6. `tests/shots/m6_damage_number_tiers.shot` produces a contact sheet
   PNG; manual visual review confirms the 5 color tiers read distinctly
   and the spew/bounce/fade arcs are visible.
7. A 90-second manual real-play session on Concourse with 4 bots,
   Microgun loadout: numbers spawn on every hit, bounce-tink audibly
   on the floor, fade out cleanly. No pool-overflow warnings in the
   log, no frame-rate drop in the `--perf-overlay` FPS readout.
8. `TRADE_OFFS.md` has the two pre-disclosed entries from §15 added.
9. `CURRENT_STATE.md` and `CLAUDE.md` reflect the feature shipping.
10. The phase footnote in §19 lists each landed phase + commit date.

---

## 19. Status footnote (append when work lands)

- Phase 0 — design document drafted 2026-05-15.
- Phase 1 — Audio asset + `SFX_DAMAGE_TINK` SFX id, LANDED 2026-05-15.
  Sourced `kenney_impact-sounds/impactMetal_light_000.ogg` → 180 ms
  capped + 50 ms fade-out via the existing source_map.sh / normalize.sh
  pipeline. 50 SFX entries total (47 base + 2 M6 P02 ignition + 1 P04
  tink).
- Phase 2 — FxPool extension, LANDED 2026-05-15. `FX_DAMAGE_NUMBER`
  enum + `angle`/`ang_vel` fields on `FxParticle` (+8 B/particle);
  `MAX_BLOOD` cap 8000 → 8500 (+200 headroom above the worst combined
  jet+damage case).
- Phase 3 — Spawn function + tier table, LANDED 2026-05-15.
  `DamageTier` enum, `g_dmg_tier[]` table, `dmg_tier_for(u8)` helper,
  `fx_spawn_damage_number(pool, pos, dir, dmg, weapon_id, rng)` with
  `SHOT_LOG("dmgnum spawn pos=… dmg=N tier=T")` for paired-test
  assertions. Tier font_px ramp tuned through three iterations
  (10..18 spec → 14..32 → final 20..44 after user readability
  feedback on 4K).
- Phase 4 — `fx_update` physics branch, LANDED 2026-05-15. Gravity
  with a 0.30 s hover-ramp (number pops + hangs before falling),
  ±1 rad/s gentle tumble (was ±3 rad/s in the original spec — too
  fast to read at 60 Hz), 0.99 air drag, per-axis bounce up to 2×,
  audio_play_at(SFX_DAMAGE_TINK) on contact.
- Phase 5 — Render pass, LANDED 2026-05-15. `fx_draw_damage_numbers`
  wired into `renderer_draw_frame` after the upscale blit + before
  `hud_draw`. Font size scales by `camera.zoom * blit_scale` so glyphs
  feel world-attached (the spec's "1× zoom" calculation assumed cam
  zoom 1.0, but actual is 1.4; the multiplier closes that gap).
  4-pass cardinal-offset outline.
- Phase 6 — Single-process color-tier test, LANDED 2026-05-15. New
  `damage <mech_id> <part> <amount>` shotmode directive (added to
  both standalone + host-side event switches); `tests/shots/
  m6_damage_number_tiers.shot` exercises all 5 tiers on slipstream,
  contact sheet composites pre_damage / spawn / midflight / settled
  frames.
- Phase 7 — Hook into the two damage paths, LANDED 2026-05-15. Spawn
  fires from `mech.c::mech_apply_damage` (server / single-process) +
  `net.c::client_handle_hit_event` (multiplayer client). weapon_id
  passed as 0 from both sites (v1 ignores it per §3.1).
- Phase 8 — Paired-process sync test, LANDED 2026-05-15. New
  `make test-damage-numbers` target runs `tests/shots/net/
  run_damage_numbers.sh` on aurora (open flat-floor map per user
  feedback — slipstream's basement walls blocked the hitscan path).
  5/5 assertions pass: host spawns 3 dmgnum lines, client spawns 3
  matching lines, dmg + tier columns diff cleanly between the two
  logs.
- Phase 9 — Baselines + docs, LANDED 2026-05-15. All baselines pass
  (test-physics / test-level-io / test-snapshot / test-spawn /
  test-prefs / test-pickups / test-ctf / test-map-chunks /
  test-map-registry / test-mech-ik / test-pose-compute /
  test-frag-grenade / test-riot-cannon-sfx / test-damage-numbers,
  plus tests/net/run.sh 13/13 and tests/net/run_3p.sh 10/10). Status
  added to `CLAUDE.md`, `CURRENT_STATE.md`, `TRADE_OFFS.md`
  (two new entries per §15).
