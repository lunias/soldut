# M6 P13 — Bot AI refresh for the M6 weapon/movement set

## Status

Plan, not built. Branch `lunias/m6-bot-ai-rework` is set up off
`main` (which has the M6 P12 projectile-snapshot-sync merge).
Implementer: pick this up cold with no surrounding chat context.

## Trigger

M6 has shipped a stack of mechanic + sync changes since the bot AI
was last validated end-to-end:

- **P11 / P12 frag rework** (`documents/m6/11-frag-grenade-tuning.md`,
  `12-projectile-snapshot-replication.md`): the frag grenade is now
  a hold-to-charge throw (`WFIRE_THROW`). Press starts charging,
  release fires at a speed scaled by accumulated charge. The
  pre-existing bot fire path (`src/bot.c:2487` in the motor block)
  checks `wpn->charge_sec > 0.0f` to decide "hold the button" — but
  frag's `charge_sec` is `0.0f` (it's a release-edge weapon, not a
  pre-fire-charge weapon like the Rail Cannon). So bots fire frags
  at the minimum charge (≈0.5× velocity, 350 px/s baseline) which
  reads as a weak short lob. They never throw a real distance.

- **M6 P12 projectile snapshot replication** (`12-...md`): bouncy
  AOE projectiles now ride the snapshot stream. Server-side bot
  fire is unaffected (bots run authoritatively), but their target
  prediction should treat thrown grenades as a ballistic arc, not
  a straight-line projectile.

- **Atmospheric zones** (`09-editor-runtime-parity-and-atmospherics
  .md`): WIND / ZERO_G / ACID / FOG rects nudge or damage mechs
  inside them. Bots have no concept of these zones — they path
  through ACID like it's empty, ignore WIND drift, etc.

- **Grapple hook + the M6 jet model** are already integrated into
  bot.c (Phase 5 work from `05-bot-ai-improvements.md`). They
  should still work, but verify with the test suite below.

A 1v1 / 2v2 bake sweep against the current bot AI is expected to
show:

- Bots only ever throw frags at minimum charge.
- Bots stand in ACID zones taking environmental damage instead of
  routing around.
- Mass Driver / Plasma Cannon (other AOE weapons) — verify their
  lead-time calculation still matches the new projectile travel
  physics (drag tuning changed in `weapons.c`).

## Goal

Refresh the bot AI so it uses the current weapon and movement
mechanics competently across:

- All 14 weapons (`WeaponId` in `src/weapons.h`) — each fire kind
  handled correctly (HITSCAN / PROJECTILE / SPREAD / BURST /
  THROW / MELEE / GRAPPLE).
- All 8 ship maps (Aurora, Catwalk, Citadel, Concourse, Crossfire,
  Foundry, Reactor, Slipstream).
- All 4 bot tiers (Recruit / Veteran / Elite / Champion) producing
  a visibly distinguishable skill curve.
- All 3 match modes (FFA / TDM / CTF).
- The 4 atmospheric zone kinds (avoid ACID, lean into wind /
  zero-G when useful, see through FOG).

## Concrete known gap (start here)

`src/bot.c:2487–2498` — the motor's fire block. The hold detection
uses `wpn->charge_sec > 0.0f` only. Add an explicit
`wpn->fire == WFIRE_THROW` branch that:

- Holds `BTN_FIRE` continuously while `want_fire` is set AND the
  accumulated `m->throw_charge` is below the target charge for the
  tactical situation (Recruit ≈ 0.3, Veteran ≈ 0.6, Elite ≈ 0.9,
  Champion = full charge — values are illustrative; tune in the
  test loop).
- Releases (drops `BTN_FIRE`) when the target charge is reached, so
  `mech_try_fire` fires on the release edge.
- Estimates throw arc apex against the target's predicted position
  (gravity + drag) so the grenade actually lands near the enemy,
  not at the bot's feet.

The release-edge fire path lives at `src/mech.c:1949–1999`. The
charge accumulator is at `src/mech.c:1872+`
(`mech_predict_throw_charge` for client predict;
`mech_try_fire` for the authoritative server path bots use).

## Other capabilities to add (in this order)

1. **WFIRE_THROW handling** (above).
2. **Atmospheric zone awareness**: extend the nav graph cost or the
   tactical layer's avoidance list. ACID rects should add a
   penalty to nodes inside them; ZERO_G should bias jet usage
   (cheap upward thrust); WIND drift should adjust horizontal
   movement targets in tight platforming. Look at
   `world.level.ambis[]` (LvlAmbi records).
3. **Per-weapon engagement profile audit**: walk every weapon in
   `weapons.c` and confirm `weapon_profile_for(id)` returns a
   sensible optimal range + strafe + prefers_high for it.
   Microgun's burst behavior, Rail Cannon's pre-fire-charge,
   Burst SMG's 3-round cadence — verify each is exercised
   correctly by the motor.
4. **Tier differentiation**: at Champion tier, the bot should look
   visibly smarter than Recruit — better aim leading, more decisive
   movement, faster recovery from setbacks. Tune
   `bot_personality_for_tier()` (`bot.c:113–174`) values against
   playtest screenshots.
5. **Per-map sanity**: run the 1v1 bake sweep
   (`tests/net/run_bot_playtest.sh`-style with 60 s matches) and
   confirm at least one kill per match per map (the old metric
   from P05).

## Out of scope

- Cooperative bot squad tactics beyond what the existing CTF
  team roles handle (Carrier / Attacker / Defender / Floater).
- Nav graph regeneration — the existing baked nav files for each
  map are authoritative. If a nav-graph gap is the actual blocker
  for a particular map, file a follow-up rather than rebaking
  inline.
- Bot personality fields (`bot.h:54–74`) beyond knob tuning —
  don't add new fields unless the existing ones are demonstrably
  insufficient.

## Acceptance

- New paired test `tests/shots/net/run_bot_frag_charge.sh`: a
  Champion-tier bot facing a stationary target on Aurora throws
  three frag grenades. Assert at least one throw is ≥ 0.9 charge
  factor (`spawn_throw mech=… charge=`) and at least one direct
  or AOE kill.
- `make test-bot-nav` still green.
- `tests/net/run_bot_playtest.sh` produces ≥ 1 mech_kill on every
  map (re-bake / re-test after any nav-cost change).
- Existing acceptance from P12 stays green:
  `make test-snapshot test-frag-grenade test-level-io test-pickups
  test-ctf test-spawn test-prefs test-map-share test-mech-ik
  test-pose-compute test-grapple-ceiling test-atmosphere-parity
  test-damage-numbers`, plus the paired-shot tests
  `tests/shots/net/run_frag_sync.sh`,
  `tests/shots/net/run_frag_charge.sh`,
  `tests/shots/net/run_frag_concourse.sh`,
  `tests/net/run.sh`.
- Side-by-side composites of bot vs. bot 1v1 on at least 3 maps
  showing tier differentiation visually.

## Estimated size

~400–700 LOC across `src/bot.c` (motor branch + per-tier knob
table + atmospheric awareness) plus a new paired test (~150 LOC).
Plan for 2 sessions including playtest iteration.
