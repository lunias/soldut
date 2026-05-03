# Reference — Soldat Constants

Raw constants extracted from the Soldat source (Pascal, MIT-licensed, github.com/Soldat/soldat). These are the **starting numbers** we tune from. They are not gospel — Soldat is 2D human-figure combat, not 2D mech combat — but they are the closest pre-existing data point we have.

Source: `https://github.com/Soldat/soldat/blob/master/shared/Constants.pas` (and per-weapon `weapons.ini` config in the original distribution).

## Movement / physics

| Constant | Value | Notes |
|---|---|---|
| `RUNSPEED` | 0.118 | per-tick velocity gain when running |
| `RUNSPEEDUP` | RUNSPEED / 6 | acceleration ramp |
| `FLYSPEED` | 0.03 | air-control sideways force |
| `JUMPSPEED` | 0.66 | impulse on jump |
| `CROUCHRUNSPEED` | RUNSPEED / 0.6 | actually slower (inverse) |
| `PRONESPEED` | RUNSPEED * 4.0 | prone is fast forward |
| `ROLLSPEED` | RUNSPEED / 1.2 | roll/dodge |
| `JUMPDIRSPEED` | 0.30 | sideways component of directional jump |
| `JETSPEED` | 0.10 | jet thrust |
| `CAMSPEED` | 0.14 | camera follow speed |
| `PARA_SPEED` | -0.5 * 0.06 | parachute vertical velocity (we don't use; mechs jet) |
| `PARA_DISTANCE` | 500 | parachute deploy distance |

## Aiming

| Constant | Value | Notes |
|---|---|---|
| `DEFAULTAIMDIST` | 7 | crosshair distance |
| `SNIPERAIMDIST` | 3.5 | sniper zoom factor |
| `CROUCHAIMDIST` | 4.5 | crouched zoom |
| `SPECTATORAIMDIST` | 30 | far for spectators |
| `AIMDISTINCR` | 0.05 | adjust per scroll |

## Collision / radii

| Constant | Value | Notes |
|---|---|---|
| `MELEE_DIST` | 12 | melee reach |
| `GUN_RADIUS` | 10 | dropped gun pickup radius |
| `BOW_RADIUS` | 20 | bow pickup radius |
| `KIT_RADIUS` | 12 | medkit pickup radius |
| `STAT_RADIUS` | 15 | stationary gun |
| `POSDELTA` | 60.0 | net position delta threshold |
| `VELDELTA` | 0.27 | velocity delta threshold |
| `MINMOVEDELTA` | 0.63 | minimum movement to send |
| `THING_PUSH_MULTIPLIER` | 9 | push force multiplier |
| `THING_COLLISION_COOLDOWN` | 60 | ticks |

## Weapon timeouts

| Constant | Value (ticks @ 60Hz) | Notes |
|---|---|---|
| `BULLET_TIMEOUT` | 7 sec = 420 | bullet life |
| `GRENADE_TIMEOUT` | 3 sec = 180 | grenade fuse |
| `M2BULLET_TIMEOUT` | 1 sec = 60 | M2 minigun bullets |
| `FLAMER_TIMEOUT` | 32 | flamer particle life |
| `FIREINTERVAL_NET` | 5 | min ticks between net-broadcast shots |
| `MELEE_TIMEOUT` | 1 | tick |
| `GUNRESISTTIME` | 20 sec = 1200 | dropped-gun decay |
| `ARROW_RESIST` | 280 | arrow decay |

## Damage modifiers

| Constant | Value | Notes |
|---|---|---|
| `M2HITMULTIPLY` | 2 | minigun hit multiplier |
| `EXPLOSION_IMPACT_MULTIPLY` | 3.75 | impulse mult on explosion |
| `EXPLOSION_DEADIMPACT_MULTIPLY` | 4.5 | impulse mult on dead bodies |
| `M2GUN_OVERAIM` | 4 | overheat aim spread |
| `M2GUN_OVERHEAT` | 18 | overheat threshold |
| `CLUSTER_GRENADES` | 3 | cluster pieces |
| `MAX_INACCURACY` | 0.5 | max bink/spread |

## Health / damage

| Constant | Value | Notes |
|---|---|---|
| `DEFAULT_HEALTH` | 150 | normal mode |
| `REALISTIC_HEALTH` | 65 | realistic mode |
| `BRUTALDEATHHEALTH` | -400 | gibs at this HP |
| `HEADCHOPDEATHHEALTH` | -90 | decapitation threshold |
| `HELMETFALLHEALTH` | 70 | helmet pops off |
| `HURT_HEALTH` | 25 | "hurt" voice line trigger |
| `DEFAULTVEST` | 100 | flak vest HP |

## Bonus items / power-ups

| Constant | Value | Notes |
|---|---|---|
| `FLAMERBONUSTIME` | 600 ticks | berserk-flamer duration |
| `PREDATORBONUSTIME` | 1500 ticks | predator (cloak) duration |
| `BERSERKERBONUSTIME` | 900 ticks | berserk duration |
| `FLAMERBONUS_RANDOM` | 5 | random spawn weight |
| `PREDATORBONUS_RANDOM` | 5 | |
| `VESTBONUS_RANDOM` | 4 | |
| `BERSERKERBONUS_RANDOM` | 4 | |
| `CLUSTERBONUS_RANDOM` | 4 | |

## Misc

| Constant | Value | Notes |
|---|---|---|
| `MAX_PUSHTICK` | 125 (client), 0 (server) | client-side push smoothing |
| `BULLETTRAIL` | 13 | trail particle count |
| `M79TRAIL` | 6 | M79 trail count |
| `BULLETLENGTH` | 21 | client-only bullet sprite length |
| `MOUSEAIMDELTA` | 30 | aim sensitivity |
| `SPAWNRANDOMVELOCITY` | 25 | spawn jitter |
| `ILUMINATESPEED` | 0.085 | flare animation |
| `CLIENTMAXPOSITIONDELTA` | 169 | client-side correction limit |
| `MAX_FOV` | 1.78 | radians ≈ 102° |
| `MIN_FOV` | 1.25 | radians ≈ 71° |

## Weapons table (Normal mode, from Soldat `weapons.ini`)

| Weapon | Damage | FireRate (ms) | Reload (ms) | Recoil | Bink | Ammo | ProjSpeed | Push |
|---|---|---|---|---|---|---|---|---|
| Desert Eagles | 1.81 | 24 | 87 | 0 | 0 | 7 | 19 | 0.0176 |
| HK MP5 | 1.01 | 6 | 105 | 0 | 0 | 30 | 18.9 | 0.0112 |
| AK-74 | 1.004 | 10 | 165 | 0 | -12 | 35 | 24.6 | 0.01376 |
| Steyr AUG | 0.71 | 7 | 125 | 0 | 0 | 25 | 26 | 0.0084 |
| SPAS-12 | 1.22 | 32 | 175 | 0 | 0 | 7 | 14 | 0.0188 |
| Ruger 77 | 2.49 | 45 | 78 | 0 | 0 | 4 | 33 | 0.012 |
| M79 | 1550 | 6 | 178 | 0 | 0 | 1 | 10.7 | 0.036 |
| Barrett M82A1 | 4.45 | 225 | 70 | 0 | 65 | 10 | 55 | 0.018 |
| FN Minimi | 0.85 | 9 | 250 | 0 | 0 | 50 | 27 | 0.0128 |
| XM214 Minigun | 0.468 | 3 | 480 | 0 | 0 | 100 | 29 | 0.0104 |
| USSOCOM | 1.49 | 10 | 60 | 0 | 0 | 14 | 18 | 0.02 |
| Combat Knife | 2150 | 6 | 3 | 0 | 0 | 1 | 6 | 0.12 |
| Chainsaw | 50 | 2 | 110 | 0 | 0 | 200 | 8 | 0.0028 |
| LAW | 1550 | 6 | 300 | 0 | 0 | 1 | 23 | 0.028 |
| Flame Bow | 8 | 10 | 39 | 0 | 0 | 1 | 18 | 0 |
| Bow | 12 | 10 | 25 | 0 | 0 | 1 | 21 | 0.0148 |
| Flamer | 19 | 6 | 5 | 0 | 0 | 200 | 10.5 | 0.016 |
| M2 MG | 1.8 | 10 | 366 | 0 | 0 | 100 | 36 | 0.0088 |
| Frag Grenade | 1500 | 80 | 20 | 0 | 0 | 1 | 5 | 0 |

Damage uses `HitMultiply`, varies by hit-location. Explosives use hitscan with AOE. Fire rate and reload in milliseconds. Bink is vertical recoil compensation in Soldat units.

## Ragdoll / particle system

From `Parts.pas`:

| Constant / behavior | Value | Notes |
|---|---|---|
| `NUM_PARTICLES` | 560 | total particle pool |
| `RKV` | 0.98 | velocity retention damping per tick |
| Verlet step | `pos += (pos - prev) * RKV + force * dt^2` | classic |
| Constraint type | distance (stick) | rest length stored |
| Solver | Gauss-Seidel relaxation | iterations: ~8 |
| Active flag per particle | true/false | inactive = skip |
| Mass | `OneOverMass[i]` (inverse) | 0 = pinned |

## Map collision (PolyMap.pas)

| Constant / behavior | Value | Notes |
|---|---|---|
| `MAX_POLYS` | 5000 | per map |
| Polygon shape | triangle | 3 vertices each |
| Per-poly metadata | type, perp[3], bounciness | precomputed normals |
| Spatial partition | sectors | grid -35..+35 |
| Collision test | point-in-poly + closest perpendicular | for push-back |
| Polygon types | 26 kinds | normal, ice, deadly, lava, team-color, etc. |

## Animation (Anims.pas)

- Animations are **`.poa` files**: keyframe poses on a 20-joint skeleton.
- 44 named animations: Stand, Run, RunBack, Jump, JumpSide, Fall, Crouch, CrouchRun, Reload, Throw, Recoil, SmallRecoil, Shotgun, ClipOut, ClipIn, SlideBack, Change, ThrowWeapon, WeaponNone, Punch, ReloadBow, Barret, Roll, RollBack, CrouchRunBack, Cigar, Match, Smoke, Wipe, Groin, Piss, Mercy, Mercy2, TakeOff, Prone, Victory, Aim, HandsUpAim, ProneMove, GetUp, AimRecoil, HandsUpRecoil, Melee, Own.
- Each animation has Frames (positions per joint), NumFrames, Speed (1-9), Loop flag.
- Speed values mostly 2-4.

## Conversion notes for our game

- Soldat's tick is 60 Hz; we adopt the same simulate rate.
- Soldat units are *not* pixels — multiply by ~6 for px-equivalent (`PARTICLE_SCALE` in Soldat).
- Soldat damage values are multipliers against weapon `HitMultiply`. We are flatter — base damage in HP, multiplied by hit-location.
- Soldat's `RUNSPEED = 0.118` per tick = ~7.1 units/sec; in our game, "Trooper" runs at 200 px/s baseline.
- Our jet uses Soldat's `JETSPEED = 0.10` as starting point but parameterized per chassis.
- Soldat does NOT have body-vs-body collision; we adopt this exactly.
- Soldat's animation system is pose-driven and we copy this approach; our format is simpler binary (no separate `.poa` files).

## What we DON'T inherit

- The `.PMS` map format — we have our own `.lvl` (see [07-level-design.md](../07-level-design.md)).
- The Pascal source — we are in C.
- The 26-polygon-type system — we have a tighter set (SOLID, ICE, DEADLY, ONE_WAY, BACKGROUND).
- The 44-animation set — ours is 24, scoped to mech motion.
- The team-color polygon mechanic — our maps are not team-tinted.
- The parachute (`PARA_SPEED`, `PARA_DISTANCE`) — mechs use jets.
