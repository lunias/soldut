# P19 — Audio sourcing log

This document was originally drafted as a shopping list for the
sourcing pass. After P19 shipped (2026-05-12) it's the record of
what was actually sourced and where, plus the keyword
guidance an operator would use to re-source a slot if a sample
needed replacement.

The canonical mapping (which source file maps to which manifest
entry) lives in `tools/audio_inventory/source_map.sh` — that's the
script `make` doesn't run automatically, but is the executable
spec for re-materializing every asset under `assets/sfx/` and
`assets/music/`. The credits attribution lives in
`assets/credits.txt` under the AUDIO section.

## Sources used

All CC0. No account creation required.

- **Kenney.nl** — six asset packs zipped under
  `assets/raw/audio/kenney_*.zip`, extracted under
  `assets/raw/audio/extracted/`. Packs: `sci-fi-sounds`,
  `impact-sounds`, `digital-audio`, `ui-audio`,
  `interface-sounds`, `music-jingles`.
- **opengameart.org** — seven CC0 music tracks under
  `assets/raw/audio/oga_music/`, downloaded by URL from
  `https://opengameart.org/sites/default/files/<filename>`.

Total sourced: ~30 MB of pack/zip data in `assets/raw/audio/`
(gitignored alongside the existing `assets/raw/` convention),
producing ~8 MB of shipped audio under `assets/sfx/` +
`assets/music/`.

## Slot mapping (47 SFX + 1 servo + 14 music/ambient)

The full table is in `tools/audio_inventory/source_map.sh`.
The shape of the mapping:

### Weapons (14) — keyword "laser / energy / projectile launch"

Every weapon SFX maps to a Kenney `sci-fi-sounds` laser / thruster /
impact variant. Distinct silhouettes (Pulse Rifle vs Plasma SMG
vs Mass Driver) drop into different Kenney slots: laserLarge_NNN
for high-energy weapons, laserSmall_NNN for sidearms, laserRetro_NNN
for chunky-fire weapons, thrusterFire_NNN for the propellant-driven
Mass Driver and Micro-Rockets, impactMetal_NNN for melee strikes.

### Hits (3) — keyword "impact / wet / metal"

- `hit_flesh.wav`    ← Kenney sci-fi `slime_000.ogg` (organic squelch)
- `hit_metal.wav`    ← Kenney sci-fi `impactMetal_000.ogg`
- `hit_concrete.wav` ← Kenney impact `impactGeneric_light_000.ogg`

### Explosions (3) — keyword "low rumble / explosion crunch"

- `explosion_large.wav`  ← `lowFrequency_explosion_000.ogg` (heaviest)
- `explosion_medium.wav` ← `explosionCrunch_000.ogg`
- `explosion_small.wav`  ← `explosionCrunch_002.ogg`

### Movement (7 + servo)

- Footsteps: Kenney impact-sounds `footstep_concrete_NNN`,
  `footstep_snow_NNN` (used for ice variant), and the lightest
  `impactMetal_light_000.ogg` for the metal-surface footstep.
- Jet pulse / boost: `thrusterFire_002.ogg`, `spaceEngineLarge_000.ogg`
- Landing hard / soft: `impactPlate_heavy_000.ogg`, `impactSoft_medium_000.ogg`
- Servo loop: `spaceEngine_000.ogg` (continuous engine drone)

### Pickups (7) — keyword "powerup / chime / two-tone"

All from Kenney `digital-audio`. Distinct tonal moves:
`powerUp1` (health), `twoTone1` (ammo), `powerUp4` (armor),
`threeTone1` (weapon), `powerUp7` (powerup),
`zap1` (jet fuel), `twoTone2` (respawn).

### Grapple (4) — keyword "swoosh + impact + retract"

- `grapple_fire.wav`      ← `laserSmall_003.ogg`
- `grapple_hit.wav`       ← `impactMetal_medium_001.ogg`
- `grapple_release.wav`   ← `doorClose_000.ogg` (mechanical click)
- `grapple_pull_loop.wav` ← `forceField_000.ogg` (cable tension)

### Flag (4) — keyword "phase up/down + capture jingle"

- `flag_pickup.wav`  ← `phaseJump1.ogg`
- `flag_drop.wav`    ← `phaserDown1.ogg`
- `flag_return.wav`  ← `phaserUp1.ogg`
- `flag_capture.wav` ← `jingles_HIT07.ogg` (music-jingles Hit set)

### UI (3) — keyword "click / hover / toggle"

- `ui_hover.wav`  ← Kenney ui-audio `rollover1.ogg`
- `ui_click.wav`  ← Kenney ui-audio `click1.ogg`
- `ui_toggle.wav` ← Kenney interface-sounds `toggle_002.ogg`

### Death (2) — keyword "fanfare + grunt"

- `kill_fanfare.wav` ← `jingles_HIT15.ogg`
- `death_grunt.wav`  ← `lowFrequency_explosion_001.ogg` (sub-bass thud)

Voice / vocal grunt was rejected per the brief's
"Voice / chatter / announcer — kept silent at v1" rule — the
death cue is a sub-bass thud, not a "ugh."

### Music (7) — opengameart CC0

| Map slot | OGA page | Artist / title |
|---|---|---|
| foundry.ogg (+ Crossfire shared) | [/content/bleak-terminal](https://opengameart.org/content/bleak-terminal) | ruskerdax — *Bleak Terminal Theme* |
| maintenance.ogg | [/content/approach](https://opengameart.org/content/approach) | ruskerdax — *Approach* |
| reactor.ogg | [/content/infiltration](https://opengameart.org/content/infiltration) | Nicole Marie T — *Infiltration* |
| atrium.ogg | [/content/atmospheric-puzzles](https://opengameart.org/content/atmospheric-puzzles) | *Atmospheric Puzzles* |
| exterior.ogg | [/content/last-stand-in-space-looped](https://opengameart.org/content/last-stand-in-space-looped) | *Last Stand in Space (looped)* |
| aurora.ogg | [/content/blue-ants](https://opengameart.org/content/blue-ants) | *Blue Ants* |
| citadel.ogg | [/content/dungeon-ambience](https://opengameart.org/content/dungeon-ambience) | *Dungeon Ambience* |

Each track is re-encoded to mono Vorbis q4 22050 Hz (~128 kbps)
via ffmpeg in `source_map.sh::mkogg_music`, then run through
`-23 LUFS loudnorm` by the `audio-normalize --loudness` pass.

### Ambient loops (7)

All seven derive from Kenney `sci-fi-sounds` engine / computer /
force-field samples, stream-looped to ~30 s with 50 ms fades at
the seam:

- `ambient_foundry.ogg`     ← `engineCircular_000.ogg`
- `ambient_maintenance.ogg` ← `spaceEngine_001.ogg`
- `ambient_reactor.ogg`     ← `computerNoise_000.ogg`
- `ambient_atrium.ogg`      ← `forceField_001.ogg`
- `ambient_exterior.ogg`    ← `spaceEngineLow_000.ogg`
- `ambient_aurora.ogg`      ← `forceField_002.ogg`
- `ambient_citadel.ogg`     ← `spaceEngineLow_002.ogg`

Crossfire shares Foundry's kit short_name in `tools/cook_maps/
cook_maps.c::build_crossfire::set_meta(..."foundry"...)`, so no
ambient_crossfire slot exists.

## Re-pinning a slot

1. Pick a new source: another Kenney variant, an opengameart track,
   or a Soldut-original recording.
2. Edit the `mkwav` / `mkogg_*` line in
   `tools/audio_inventory/source_map.sh`.
3. Run `bash tools/audio_inventory/source_map.sh` to re-materialize.
4. Append a credits row in `assets/credits.txt`.
5. `make audio-credits` confirms coverage.
6. `make test-audio-smoke` confirms the runtime accepts the result.

## Known omissions vs the spec

`documents/m5/09-audio.md` originally listed `footstep_metal_*.wav
× 4 variants` (and similar per-surface counts) — i.e. four
multi-variant footsteps per surface for blending. The runtime
manifest in `src/audio.c::g_sfx_manifest` only declares one slot per
surface; per-surface variation lives in the 5-alias rotation
(`alias_count = 5`) which retriggers the same sample with internal
voice rotation. That mismatch is a design simplification that
landed at P14; the multi-variant per-surface footstep is not
something P19 needed to source.

`SFX_MECH_SERVO_LOOP` is documented in `src/audio.h` but isn't a
manifest slot — the servo plays via a dedicated `Sound g_servo`
loaded from the path returned by `audio_servo_path()`. P19
materializes `assets/sfx/mech_servo_loop.wav` so that loader hits.
