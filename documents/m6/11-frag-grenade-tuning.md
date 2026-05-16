# M6 — Frag grenade tuning + camera follow

## What changed

Reworked the frag grenade from a press-fire AOE into a Worms-style
hold-to-charge throw with parabolic physics, beefier visuals, and a
camera that actually follows the projectile. Shipped across seven
commits (`c23e421` → `0138a23`) iterating against play-test feedback
on real interactive 1v1 sessions.

## Final mechanic

**Hold to charge, release to throw.** The button that fires the
frag — LMB when frag is the active slot, RMB when it's in the other
slot — accumulates `Mech.throw_charge` in seconds while held (max
`FRAG_CHARGE_MAX_SEC = 1.0 s`). Release fires the throw at speed
scaled by the accumulated fraction. The charge meter renders from
R_HAND toward the reticle in world space, length proportional to
charge, color cyan → yellow → red as it approaches max.

**Direct aim.** The aim direction IS the launch direction. Worms-
style player control: aim ~45° up for max range, aim higher to clear
a wall (more arc, less range), aim AT a close target to drop on it.
A small constant 0.12 (~7°) upward bias keeps perfectly horizontal
aim from depositing the grenade at the player's feet.

**Physics.** Gravity 1.05× world default, drag 0.12 per second.
Bouncy with 30 % normal-component absorption per bounce, 10 %
tangential friction. Settled-detonate when post-bounce |v| <
`FRAG_SETTLED_VMAG_PXS = 80 px/s` (avoids the "grenade rolls to a
stop, sits for half a second, then explodes" pause). Fuse 2.4 s.

**Hit response.** Direct mech-bone contact via swept-segment test
(22 px capsule for frag, wider than bullets because the player
expects a grenade that VISUALLY touches an opponent to detonate)
applies 60 dmg through `mech_apply_damage` THEN detonates the AOE
(100 dmg / 240 px radius). On flat ground a clean face-hit at point
blank lands 60 + ~60 = 120 dmg uncapped, ~96 dmg through Light armor.

## Tunable table

| Tunable                           | Value     | Where                  |
|-----------------------------------|----------:|------------------------|
| `FRAG_CHARGE_MAX_SEC`             | 1.0 s     | `weapons.h`            |
| `FRAG_THROW_SPEED_MIN_MUL`        | 0.5×      | `weapons.h`            |
| `FRAG_THROW_SPEED_MAX_MUL`        | 4.0×      | `weapons.h`            |
| `FRAG_SETTLED_VMAG_PXS`           | 80 px/s   | `weapons.h`            |
| `FRAG_EXPLOSION_LINGER_TICKS`     | 40        | `weapons.h`            |
| frag `.damage` (direct)           | 60        | `weapons.c`            |
| frag `aoe_radius / aoe_damage`    | 240 / 100 | `weapons.c`            |
| frag `aoe_impulse`                | 65        | `weapons.c`            |
| frag `projectile_speed_pxs`       | 700       | `weapons.c` (baseline) |
| frag `projectile_life_sec`        | 2.4 s     | `weapons.c`            |
| frag `projectile_drag`            | 0.12      | `weapons.c`            |
| frag `projectile_grav_scale`      | 1.05      | `weapons.c`            |
| upward aim bias (constant)        | 0.12      | `weapons.c`            |
| bone-collision radius (frag)      | 22 px     | `projectile.c`         |
| bounce damping (perp / parallel)  | 0.70 / 0.90 | `projectile.c`       |
| camera grenade-follow blend / cap | 0.95 / 1400 px | `render.c`        |

Max speed at full charge = 700 × 4.0 = **2800 px/s** (faster than
the 1900 px/s rifle bullet). At a 45° aim that's ~3900 px no-drag
range; with drag (0.12) actual reachable distance is ~3000 px on
flat ground — about 2/3 of a Reactor width per throw.

## Camera

Two layers stacked on the existing smooth-follow:

1. **Grenade follow** — when the local mech has a thrown frag in
   flight, focus shifts toward it (blend 0.95, capped 1400 px). The
   mech may temporarily leave the viewport on long throws; the
   action IS the grenade.
2. **Explosion linger** — after detonate, `world.last_explosion_pos`
   drives focus for `FRAG_EXPLOSION_LINGER_TICKS = 40` (~0.67 s) so
   the FX register clearly, then smooth-follow pans home in a
   single continuous motion.

`detonate()` writes `last_explosion_pos` directly (not only inside
`explosion_spawn`) so client-side paths that skip the spawn for
bouncy projectiles still have a continuous focus point.

## FX

Each explosion now spawns:

- **12 large bright-orange flash particles** (5-8 px, 0.28 s life)
  — reads as the fireball instant.
- **36 fine shrapnel sparks** (was 28) — wide fragment cone.
- **7 lingering smoke puffs** with upward + outward bias (the prior
  "smoke loop" was a no-op — variables computed but never spawned).

Plus the existing tiered screen shake (+0.60 at ≥100 dmg, +0.50 at
≥50, +0.40 below). Hard to miss against any background.

## Network sync

Bouncy projectiles diverge between client and server sims over the
2.4 s fuse — collision tests against mech bones produce different
hit / miss results because remote-mech bones on the client lag by
snapshot interp, and the bounce code is sensitive to small velocity
differences.

Current workaround (`net.c::client_handle_explosion`):

- Client predicts its own AOE detonations, pushes an
  `EXPL_SRC_PREDICTED` record (dedupe window 600 ticks to cover
  bouncy-fuse divergence + shot-mode tick drift).
- Wire-receive finds the record and dedupes — no double FX, no
  `last_explosion_pos` overwrite.
- When no predict matches (server detonated first / divergence
  larger than the window), the client snaps the local projectile's
  pos to the server's broadcast pos for the dying frame so the
  sprite visibly "slides" to where the explosion actually fires.
  Small divergence: invisible. Large divergence: grenade rapidly
  translates to the true detonation point, then booms — at least
  the visual lines up with damage.

**Damage is server-authoritative** at the server's grenade
position. With the visual-snap, FX center == damage center.

**The clean long-term fix** is per-tick projectile position
replication via snapshot so client and server simulations stay
locked. Tracked separately in
[`12-projectile-snapshot-replication.md`](12-projectile-snapshot-replication.md).

## Sprite

Replaced the original 4 px yellow ball with:

- 6 px olive body + 7 px dark outline + 2 px highlight (volume).
- Lever cap that orbits opposite to the velocity vector (reads as
  tumbling without a sprite atlas).
- Pulsing red fuse spark at 12 Hz so the projectile tracks easily
  against busy / foggy backgrounds.

Dying-frame: `detonate()` keeps `alive = 1` for one render frame
after firing the explosion, with `render_prev` snapped to `pos`, so
the grenade sprite sits AT the explosion center for that frame with
the spark FX fanning out from the same point. Kill happens at the
top of `projectile_step` on the following tick.

## FFA damage carve-out

`lobby_init` seeds every slot with `team = MATCH_TEAM_FFA = 1`,
numerically identical to `MATCH_TEAM_RED`. Both
`projectile.c::explosion_spawn`'s AOE loop and
`mech.c::mech_apply_damage` were using
`shooter->team == m->team && !w->friendly_fire` to skip damage.
With `friendly_fire = false` default and every FFA slot on team 1,
the check silently blocked ALL inter-player damage in FFA — every
weapon, not just grenades.

Both check sites now bypass the friendly_fire gate when
`world.match_mode_cached == MATCH_MODE_FFA`. There are no friends
in FFA. TDM / CTF unchanged.

## Tests

- `tests/shots/net/run_frag_charge.sh` (9/9 PASS) — paired throws
  on Aurora with both peers visible to each other via `peer_spawn`.
  Asserts charge factors tier, velocities scale (≤ 2000 / ≥ 2400
  px/s), wire round-trip delivers.
- `tests/shots/net/run_frag_concourse.sh` (6/6 PASS) — actual
  grenade battle on Concourse, both peers throw 3 each across the
  main floor. Asserts no double-spawned FX (dedupe works), real
  damage application visible in the host log.
- `tests/shots/m6_frag_solo.shot` — single-mech reactor diagnostic
  with tight per-tick captures around launch / arc / impact /
  linger / pan-back.
- Existing `tests/shots/net/run_frag_grenade.sh` (wan-fixes-10
  regression) — regex relaxed to not pin the old `r=140.0` radius.
  Occasionally flakes 7/9 on shot-mode wire-event timing; unrelated
  to gameplay logic.

## Open work

- Per-tick projectile snapshot — see
  [`12-projectile-snapshot-replication.md`](12-projectile-snapshot-replication.md).
- Bots are unaware of the hold-to-charge mechanic (`bot.c` still
  press-fires on edge). They'll throw at min charge → short tap
  lobs. Acceptable for a friends-ship build; queue a follow-up to
  give bots a "hold N ticks then release" pattern.
- Frag is currently the only WFIRE_THROW weapon. If a second one
  ships (heavier grenade variant, smoke?), the
  `FRAG_LOB_*` / `FRAG_THROW_SPEED_*` constants need per-weapon
  fields on `Weapon` rather than file-scope #defines.
