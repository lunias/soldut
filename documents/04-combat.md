# 04 — Combat

This document specifies what "shooting" means in this game: weapons, damage, recoil, the bink/self-bink interaction, blood, and gore. Numbers are starting targets — they will be tuned, but the *system* is fixed by this document.

## The shape of the moment

When you press fire:

1. **Tick 0**: input registers. Server validates fire-rate gate. If allowed, a bullet (or hitscan ray) is spawned on the server.
2. **Tick 0**: client predicts the same — spawns a tracer locally so it feels instant.
3. **Same tick**: recoil impulse is applied to your mech's hand particle. Your skeleton shudders.
4. **Tick 0–N**: muzzle flash, casing eject (if applicable), shell eject (if applicable), audio cue.
5. **Tick T**: the bullet hits something (T = 0 for hitscan, T = travel time for projectile).
6. **On hit**: damage, decal, blood, impulse to the hit body's part, kill-feed event, audio at hit location.
7. **If lethal**: ragdoll mode, possible dismemberment, death effects.

This is what happens, every shot. The system specified below is the implementation of those bullet points.

## Weapons (primaries)

Eight at v1. We borrow Soldat's principle that **each weapon is a different play style**, not a different DPS column. Stats below are starting targets; the *identity* is the contract.

| Weapon | Type | Damage | Fire Rate (ms) | Reload (ms) | Mag | Speed (px/tick) | Recoil | Bink | Identity |
|---|---|---|---|---|---|---|---|---|---|
| **Pulse Rifle** | hitscan | 18 | 110 | 1500 | 30 | — | low | low | The default. Versatile, accurate, forgettable on purpose |
| **Plasma SMG** | projectile | 10 | 60 | 1300 | 40 | 30 | low | mid | High RoF, low per-shot, cone spread when sprinting |
| **Riot Cannon** | projectile (pellet x6) | 8 (×6) | 350 | 1700 | 6 | 25 | high | high | Shotgun. Lethal close, useless past 250 px |
| **Rail Cannon** | hitscan | 95 | 1200 | 2200 | 4 | — | huge | very high | Sniper. Charges 0.4s before fire |
| **Auto-Cannon** | projectile | 14 | 80 | 1800 | 60 | 28 | mid | mid | LMG. Suppressive, terrible if you move |
| **Mass Driver** | projectile (slow) | 220 (impact) + AOE | 1100 | 3000 | 1 | 14 | huge | huge | Rocket launcher. Self-damage, area denial |
| **Plasma Cannon** | projectile (medium) | 60 + small AOE | 700 | 2200 | 8 | 20 | mid | mid | Anti-armor. Glowing arc, slow |
| **Microgun** | projectile | 6 | 25 | 4500 | 200 | 30 | low | low | Bullet hose. 0.5s spin-up, melts armor |

## Weapons (secondaries)

Six at v1.

| Weapon | Type | Damage | Fire Rate (ms) | Reload (ms) | Mag/Charges | Identity |
|---|---|---|---|---|---|---|
| **Sidearm** | hitscan | 25 | 200 | 800 | 12 | The reliable backup |
| **Burst SMG** | projectile (3-burst) | 12 | 70 (between bursts: 350) | 1400 | 24 | Spike damage, rhythmic |
| **Frag Grenades** | thrown projectile | 80 + AOE | 600 | — | 3 | Lobs, bounces, 1.5s fuse |
| **Micro-Rockets** | projectile | 35 + tiny AOE | 250 | 2200 | 5 | Spammy, no falloff |
| **Combat Knife** | melee | 90 | 200 | — | ∞ | Backstab kills (×2.5 from behind) |
| **Grappling Hook** | utility | 0 | 1200 | — | 1 | Anchors, pulls; 600 px range |

## Hitscan vs projectile

- **Hitscan** = ray test on the tick the trigger is pulled. Used for fast/small-mag rifles (Pulse Rifle, Rail Cannon, Sidearm). Server-side rewinds with lag compensation (see [05-networking.md](05-networking.md)).
- **Projectile** = a Verlet particle in the simulation that travels at finite speed and collides every tick. Used for everything visible-flying. Bullets have:
  - position, velocity
  - lifetime (timeout in ticks; default 7 seconds × 60 = 420 ticks, matching Soldat's `BULLET_TIMEOUT`)
  - owner (so we don't friendly-fire ourselves at point-blank)
  - drag (negligible for fast bullets, important for grenades)

## Damage model

```c
final_damage = base_damage * hit_location_mult * armor_mult * distance_mult;
```

Hit-location multipliers (per the bone segment hit):

| Body part | Multiplier |
|---|---|
| Head | ×1.6 |
| Chest / pelvis | ×1.0 |
| Arms | ×0.7 |
| Legs | ×0.7 |
| Hands / feet | ×0.5 |

Armor reduction:

```c
if (armor > 0) {
    float absorbed = min(damage * armor.absorb_ratio, armor.hp);
    damage -= absorbed;
    armor.hp -= absorbed;
    if (armor.hp <= 0) drop_armor(mech);
}
```

Armor `absorb_ratio` is per-tier: Light 0.4, Heavy 0.6, Reactive 0.5 (and 1.0 for one explosion). When armor breaks, it falls off as a visible polygon.

Distance falloff (projectile only):

```c
float falloff = clamp(1.0f - (dist / weapon.falloff_max), weapon.falloff_floor, 1.0f);
```

Most weapons have no falloff (`falloff_max = INF`). Riot Cannon and Sidearm do. Hitscan is full-damage at any range (the long hitscan rifles are balanced by fire rate and recoil).

## Recoil & bink

Two distinct effects, both inherited from Soldat's design. **Recoil** is what your weapon does to *you*. **Bink** is what other people's bullets do to *your aim*.

### Recoil

When a weapon fires, an impulse is applied to your hand particle:

```c
hand.pos += -aim_dir * weapon.recoil_impulse;
```

This jolts the hand back, which ripples through the constraint solver to the elbow, shoulder, chest. The next shot you fire while the body is still settling has a higher angular error — naturally, mechanically, without a "spread cone" simulation. **The recoil is the spread.** You can mitigate by firing slower or by being grounded (more mass anchored, less ripple).

This is the same trick that makes the Soldat gostek's recoil feel right and is the killer feature of using a unified physics body for the player.

### Bink (incoming-fire flinch)

When a bullet *passes near* your mech (within ~80 px) or hits you, a small impulse is applied to your **aim** — a transient angular nudge:

```c
mech.aim_angle += sign(close_pass_side) * weapon.bink_value * (close ? 1.0f : 0.5f);
```

Bink is per-weapon (`weapon.bink` column above). Heavy weapons (Rail Cannon) bink hard; SMG fire bink lightly. You can ride out bink by stopping your own fire briefly (it decays exponentially); or you can *embrace it* and accept your shot's randomness will widen.

### Self-bink

You also bink yourself when you fire fast — a small aim-jitter when your fire rate is faster than your weapon's "stable" rate. This is what keeps you from holding a microgun on someone's head from across the map.

```c
mech.aim_angle += (random_signed() * weapon.self_bink) * (current_rof / weapon.stable_rof);
```

These three (recoil, bink, self-bink) are the **entire spread system**. There is no random cone bloom. The randomness is in the body simulation and the aim-jitter, both of which are visible to the player and feel intentional.

## Reloading

Reload is an animation that takes `weapon.reload_ms`. While reloading you cannot fire. Cancellation: switching to your secondary cancels the reload and the magazine is dropped (you waste those bullets). This is explicit so players can panic-swap.

## Melee

A single melee hit is a hitscan ray of length 60 px from the mech's chest, in the aim direction, on the tick the melee button is pressed. Damage applies if any opponent's bone segment intersects. Backstab (hit from behind, dot product of attacker's aim and victim's facing > 0.5) does ×2.5 damage.

Knife throwing: alternate-fire (right-click) launches the knife as a projectile at 25 px/tick, sticks where it hits (kill if it sticks in head/chest, ~50 damage to limb). Throwing the knife leaves you without a secondary until you pick it back up.

## Explosions

Used by Mass Driver, Frag Grenades, Plasma Cannon (small), and self-detonation effects. An explosion at point `P` with radius `R` and base damage `D`:

1. **Damage**: for every mech whose body bounding box overlaps `R`:
    - For each particle in that mech, compute `dist = length(particle - P)`.
    - If `dist < R`: damage that particle's parent body part by `D * falloff(dist/R)`.
    - Falloff curve: `1 - (dist/R)^2`.
    - Line-of-sight check: cast a ray from P to the body center; if blocked by a SOLID poly, halve the damage. Cheap, prevents grenade kills through walls feeling unfair.
2. **Impulse**: same loop, push each affected particle `dir * power * falloff`. Blows ragdolls around.
3. **Visuals**: sprite explosion + 3 layers of additive flash + 30 spark particles + 6 smoke puffs + screen shake within 800 px.
4. **Audio**: low-end thump + transient crack, attenuated by distance.

## Blood

Blood is a particle pool with **3000 capacity** at startup. Blood particles spawn from:

- A bullet hit (5–15 particles).
- A melee hit (8–20 particles).
- A dismemberment (50–80 particles, fountain).
- An ongoing stump (a continuous emitter at the dismemberment point until the body decays).

Blood particles are 2 px circles, alpha-blended, with a velocity inherited from the impact direction plus randomness. Gravity affects them. They live ~1.5 seconds. On death, half of them spawn a **decal** at their final position.

```c
typedef struct {
    Vec2  pos, vel;
    float life;
    float size;
    uint32_t color;     // varies hue: red core, orange edges
} BloodParticle;
```

## Decals (the splat layer)

Permanent blood and burn marks live on a render-to-texture **splat layer** the size of the level (or chunked to tiles for large levels). When a blood particle dies near a surface, we render its decal sprite into the splat layer **once** and forget the particle. Decals never re-render, never have per-frame cost. A level can have thousands of blood splatters with zero ongoing perf hit. (See [06-rendering-audio.md](06-rendering-audio.md).)

Decal life is the level's life — they persist until the level ends, then we toss the splat layer.

## Gibs (limbs)

When a limb dismembers, we don't spawn it as a special object — it is **the same particles** that were part of the mech, with their downstream constraints intact and the upstream constraint deleted. The "gib" runs through the same Verlet solver, falls, bounces, lives ~30 seconds before being reaped (recycled into the particle pool when it has been off-camera for 5+ seconds). Gibs draw blood emitters at the stump for their first ~3 seconds.

## Kill feed and screen real estate

Top-right of HUD shows the last 5 kills:

```
  [ Player A ] [Pulse Rifle] [ Player B ]
  [ Player C ] [Mass Driver - HEADSHOT] [ Player A ]
```

When you kill someone, a brief kill-confirmation glyph flashes near your crosshair (no popup, no XP gain — just a clean confirmation) and the kill-feed entry pulses for 0.5s.

## Special death types (cosmetic)

- **HEADSHOT** — first hit decapitated.
- **GIB** — body lost two or more limbs in the kill blow (typical of Mass Driver / Plasma).
- **OVERKILL** — final blow exceeded 200 damage.
- **RAGDOLL** — lethal hit while in midair, body went tumbling >20 m before landing.

These trigger no gameplay effect but are tracked for the kill-feed flair.

## Friendly fire

Off in casual servers, on in tournament settings. When on:

- Damage to teammates is full-strength.
- Healing them with a repair pack still works.
- Killing a teammate = -1 to your score; the system flags repeated TKs to the host (5 TKs in 60s → auto-spectate for 60s).

## Weapon design philosophy

The numbers will move. The shape will not. Each weapon must answer:

1. **What is its range?** (close, mid, long)
2. **What is its tempo?** (single, burst, sustained)
3. **What does it punish?** (bunching, exposure, slow movement, low health)
4. **What is its weakness?** (slow reload, low ammo, high recoil, no AOE, telegraph time)

If two weapons answer those four questions the same way, one of them is redundant. We delete it. We do not stat-gate weapons (no "level 3 unlock"); the only knob is **availability** — some weapons are only on the map as pickups, not in the loadout menu, to keep them rare.

## What we are NOT doing

- **No "perks" system.** Each chassis has one passive, full stop. We don't stack 6 modifiers per kit.
- **No weapon rarity** (white/blue/purple). Either a gun is in the game or it isn't.
- **No bullet penetration.** A bullet hits the first bone segment in its path and stops. (Penetration is a balance and visual-clarity nightmare; we skip it. Mass Driver explosions handle "shoot through cover.")
- **No headshot **damage variance** based on weapon — the ×1.6 head multiplier applies uniformly. (We deviate from realism in service of legibility.)
- **No reload cancel into fire** — cancelling a reload aborts to a clean state; you can't half-reload then fire that one bullet. (This is a feel choice; revisit if it's frustrating.)
- **No gun customization** at v1. No attachments, no skins, no scope swaps. The weapon is the weapon.

## References

- Soldat `Weapons.pas` — original weapon stat table (extracted in [reference/soldat-constants.md](reference/soldat-constants.md)).
- Soldat `Constants.pas` — `BULLET_TIMEOUT`, recoil, bink constants.
- Counter-Strike's bullet-recoil model (visible recoil pattern, no random cone). We borrow the principle of *visible* recoil.
- Quake 3's hitscan rail / lightning gun balance — long-range hitscan punishment with high effective skill.
