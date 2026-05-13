#!/bin/bash
#
# tools/audio_inventory/source_map.sh — P19 mapping from CC0 sources
# to the runtime manifest paths.
#
# Reads from:
#   - assets/raw/audio/extracted/kenney_*/Audio/...   (Kenney CC0 packs)
#   - assets/raw/audio/oga_music/<kit>.{mp3,ogg}      (opengameart CC0)
#
# Writes to:
#   - assets/sfx/<entry>.wav   (the 47 SFX manifest entries)
#   - assets/sfx/ambient_<kit>.ogg
#   - assets/music/<kit>.ogg
#   - assets/sfx/mech_servo_loop.wav
#
# ffmpeg is the only external dependency (transcode + sample-rate +
# channel + bit-depth conversion in one pass, deterministic output).
# Runs idempotently — overwrites existing files unconditionally,
# but `make audio-normalize` will subsequently no-op since the
# output is already in spec.
#
# This script is the canonical record of which source file produced
# which manifest entry. To pin a different sound, edit the row and
# re-run. assets/credits.txt is updated by the companion
# write_credits.sh.

set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

K="assets/raw/audio/extracted"
M="assets/raw/audio/oga_music"
SFX="assets/sfx"
MUSIC="assets/music"

mkdir -p "$SFX" "$MUSIC"

# Re-encode SFX to 22050 Hz mono 16-bit PCM WAV.
#
# Optional 3rd arg: max duration in seconds. When set, `-t <dur>` caps
# the output and a 0.05 s fade-out is applied at the seam so the cut
# doesn't pop. This is critical for samples sourced from longer
# continuous engine / thruster recordings (kenney's thrusterFire_*,
# spaceEngine_*, lowFrequency_explosion_*) — left at full length, a
# 5-second jet pulse stacks 75× when the player holds W for 5 s.
mkwav() {
    local src="$1" dst="$2" dur="$3"
    local args="-ar 22050 -ac 1 -sample_fmt s16"
    if [ -n "$dur" ]; then
        local fade_start
        fade_start=$(awk -v d="$dur" 'BEGIN { printf "%.3f", d - 0.05 }')
        args="-t $dur -af afade=t=out:st=${fade_start}:d=0.05 $args"
    fi
    # shellcheck disable=SC2086
    ffmpeg -y -loglevel error -i "$src" $args "$dst"
}

# mkwav_loop is the same shape as mkwav but adds a 50 ms fade-in at
# the start, so the seam-to-seam wrap is silent at both ends. Used
# for samples the runtime retriggers continuously (servo,
# grapple_pull_loop) — without the fade-in, the retrigger boundary
# clicks because the start of the sample isn't at zero amplitude.
mkwav_loop() {
    local src="$1" dst="$2" dur="$3"
    local fade_start
    fade_start=$(awk -v d="$dur" 'BEGIN { printf "%.3f", d - 0.05 }')
    ffmpeg -y -loglevel error -i "$src" \
           -t "$dur" \
           -af "afade=t=in:st=0:d=0.05,afade=t=out:st=${fade_start}:d=0.05" \
           -ar 22050 -ac 1 -sample_fmt s16 "$dst"
}

# Re-encode to OGG Vorbis q3 mono 22050 Hz (ambient/loop quality).
mkogg_ambient() {
    local src="$1" dst="$2"
    ffmpeg -y -loglevel error -i "$src" -ar 22050 -ac 1 \
           -c:a libvorbis -q:a 2 "$dst"
}

# Re-encode to OGG Vorbis q4 mono 22050 Hz (music quality).
mkogg_music() {
    local src="$1" dst="$2"
    ffmpeg -y -loglevel error -i "$src" -ar 22050 -ac 1 \
           -c:a libvorbis -q:a 4 "$dst"
}

# Per-slot duration caps (seconds). Tuned for game-feel:
#   - One-shot weapon fires:  100–300 ms (cycle-rate-aware: Microgun
#     fires every 25 ms, so its sample must be ≤200 ms before alias
#     rotation can't keep up).
#   - Impact / hit / UI:      80–250 ms (percussive).
#   - Jet pulse:              150 ms (pulse rate is 67 ms in
#     `apply_jet_force` — keep the sample short so 5-alias rotation
#     covers overlap and held-jet doesn't bake into a 5-second drone).
#   - Jet boost / dash:       600 ms (single-event dash dump).
#   - Mass Driver fire:       400 ms (heavy launch + tail).
#   - Explosions:             0.5–2.0 s (event tail is part of the
#     feel — leave room for the rumble).
#   - Pickups:                ~400 ms (chime).
#   - Death grunt:            300 ms (short thud, not extended boom).
# `mkwav <src> <dst> [<max_sec>]`. Omitted = keep source duration;
# samples already ≤500 ms don't need a cap.

# ----- Weapons (14) ------------------------------------------------
mkwav "$K/kenney_sci-fi-sounds/Audio/laserLarge_000.ogg"        "$SFX/pulse_rifle.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/laserSmall_000.ogg"        "$SFX/plasma_smg.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/laserRetro_000.ogg"        "$SFX/riot_cannon.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/laserLarge_001.ogg"        "$SFX/rail_cannon.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/laserLarge_002.ogg"        "$SFX/auto_cannon.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/thrusterFire_000.ogg"      "$SFX/mass_driver.wav"        0.40
mkwav "$K/kenney_sci-fi-sounds/Audio/laserLarge_003.ogg"        "$SFX/plasma_cannon.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/laserRetro_002.ogg"        "$SFX/microgun.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/laserSmall_001.ogg"        "$SFX/sidearm.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/laserRetro_003.ogg"        "$SFX/burst_smg.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/impactMetal_001.ogg"       "$SFX/frag_throw.wav"         0.35
mkwav "$K/kenney_sci-fi-sounds/Audio/thrusterFire_001.ogg"      "$SFX/micro_rockets.wav"      0.30
mkwav "$K/kenney_impact-sounds/Audio/impactMetal_heavy_000.ogg" "$SFX/knife_melee.wav"
mkwav "$K/kenney_impact-sounds/Audio/impactMetal_medium_000.ogg" "$SFX/knife_throw.wav"

# ----- Hits (3) ----------------------------------------------------
mkwav "$K/kenney_sci-fi-sounds/Audio/slime_000.ogg"             "$SFX/hit_flesh.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/impactMetal_000.ogg"       "$SFX/hit_metal.wav"
mkwav "$K/kenney_impact-sounds/Audio/impactGeneric_light_000.ogg" "$SFX/hit_concrete.wav"

# ----- Explosions (3) ----------------------------------------------
mkwav "$K/kenney_sci-fi-sounds/Audio/lowFrequency_explosion_000.ogg" "$SFX/explosion_large.wav"  1.20
mkwav "$K/kenney_sci-fi-sounds/Audio/explosionCrunch_000.ogg"        "$SFX/explosion_medium.wav" 0.80
mkwav "$K/kenney_sci-fi-sounds/Audio/explosionCrunch_002.ogg"        "$SFX/explosion_small.wav"  0.50

# ----- Movement (7) ------------------------------------------------
mkwav "$K/kenney_impact-sounds/Audio/impactMetal_light_000.ogg"  "$SFX/footstep_metal.wav"    0.12
mkwav "$K/kenney_impact-sounds/Audio/footstep_concrete_000.ogg"  "$SFX/footstep_concrete.wav" 0.12
mkwav "$K/kenney_impact-sounds/Audio/footstep_snow_000.ogg"      "$SFX/footstep_ice.wav"      0.18
mkwav "$K/kenney_sci-fi-sounds/Audio/thrusterFire_002.ogg"       "$SFX/jet_pulse.wav"          0.15
mkwav "$K/kenney_sci-fi-sounds/Audio/spaceEngineLarge_000.ogg"   "$SFX/jet_boost.wav"          0.60
# M6 P02 — Grounded → airborne ignition cue, material-keyed.
# CONCRETE pulls from the same deep-impact family the explosion +
# death_grunt cues use (lowFrequency_explosion_000 / _001), but
# trimmed harder for the punchy "rocket-takeoff thump" character.
# ICE pulls from the thruster family (same as jet_pulse) for a
# whoosh + sibilance distinct from CONCRETE's low-end weight; reads
# as a hot exhaust hitting a cold/wet surface instead of solid floor.
mkwav "$K/kenney_sci-fi-sounds/Audio/lowFrequency_explosion_000.ogg" "$SFX/jet_ignition_concrete.wav" 0.55
mkwav "$K/kenney_sci-fi-sounds/Audio/thrusterFire_003.ogg"           "$SFX/jet_ignition_ice.wav"      0.50
mkwav "$K/kenney_impact-sounds/Audio/impactPlate_heavy_000.ogg"  "$SFX/landing_hard.wav"
mkwav "$K/kenney_impact-sounds/Audio/impactSoft_medium_000.ogg"  "$SFX/landing_soft.wav"

# ----- Servo loop (1) ----------------------------------------------
# Continuous mech-presence hum — volume is modulated by velocity at
# runtime. Source iteration: spaceEngine_000.ogg (5 s engine drone
# with strong "whwoo whwoo" pitch modulation — user-flagged as
# annoying) → forceField_002.ogg (0.95 s; retriggered ~1 Hz, which
# itself created a pulse rhythm) → computerNoise_001.ogg (5 s of
# steady electronic hum — no pitch modulation, no internal pulse).
# Trimmed to 3.0 s with 50 ms fade-in + fade-out at both seams so
# the retrigger boundary is silent (raylib `Sound` has no native
# loop — audio_servo_update PlaySound's when IsSoundPlaying goes
# false; without fade-in, the seam clicked).
mkwav_loop "$K/kenney_sci-fi-sounds/Audio/computerNoise_001.ogg" "$SFX/mech_servo_loop.wav"   3.00

# ----- Pickups (7) -------------------------------------------------
mkwav "$K/kenney_digital-audio/Audio/powerUp1.ogg"       "$SFX/pickup_health.wav"   0.45
mkwav "$K/kenney_digital-audio/Audio/twoTone1.ogg"       "$SFX/pickup_ammo.wav"     0.45
mkwav "$K/kenney_digital-audio/Audio/powerUp4.ogg"       "$SFX/pickup_armor.wav"
mkwav "$K/kenney_digital-audio/Audio/threeTone1.ogg"     "$SFX/pickup_weapon.wav"   0.55
mkwav "$K/kenney_digital-audio/Audio/powerUp7.ogg"       "$SFX/pickup_powerup.wav"
mkwav "$K/kenney_digital-audio/Audio/zap1.ogg"           "$SFX/pickup_jet_fuel.wav" 0.30
mkwav "$K/kenney_digital-audio/Audio/twoTone2.ogg"       "$SFX/pickup_respawn.wav"  0.50

# ----- Grapple (4) -------------------------------------------------
mkwav "$K/kenney_sci-fi-sounds/Audio/laserSmall_003.ogg"        "$SFX/grapple_fire.wav"
mkwav "$K/kenney_impact-sounds/Audio/impactMetal_medium_001.ogg" "$SFX/grapple_hit.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/doorClose_000.ogg"         "$SFX/grapple_release.wav"
mkwav "$K/kenney_sci-fi-sounds/Audio/forceField_000.ogg"        "$SFX/grapple_pull_loop.wav"

# ----- Flag (4) ----------------------------------------------------
mkwav "$K/kenney_digital-audio/Audio/phaseJump1.ogg"     "$SFX/flag_pickup.wav"
mkwav "$K/kenney_digital-audio/Audio/phaserDown1.ogg"    "$SFX/flag_drop.wav"
mkwav "$K/kenney_digital-audio/Audio/phaserUp1.ogg"      "$SFX/flag_return.wav"
mkwav "$K/kenney_music-jingles/Audio/Hit jingles/jingles_HIT07.ogg"  "$SFX/flag_capture.wav"   0.80

# ----- UI (3) ------------------------------------------------------
mkwav "$K/kenney_ui-audio/Audio/rollover1.ogg"          "$SFX/ui_hover.wav"
mkwav "$K/kenney_ui-audio/Audio/click1.ogg"             "$SFX/ui_click.wav"
mkwav "$K/kenney_interface-sounds/Audio/toggle_002.ogg" "$SFX/ui_toggle.wav"

# ----- Death (2) ---------------------------------------------------
mkwav "$K/kenney_music-jingles/Audio/Hit jingles/jingles_HIT15.ogg" "$SFX/kill_fanfare.wav"   0.80
mkwav "$K/kenney_sci-fi-sounds/Audio/lowFrequency_explosion_001.ogg" "$SFX/death_grunt.wav"   0.30

# ----- Music (7) — opengameart CC0 ---------------------------------
# Crossfire shares Foundry's kit; only 7 unique tracks.
for kit_entry in \
    'foundry|foundry.mp3' \
    'maintenance|maintenance.mp3' \
    'reactor|reactor.mp3' \
    'atrium|atrium.ogg' \
    'exterior|exterior.ogg' \
    'aurora|aurora.mp3' \
    'citadel|citadel.ogg' ; do
    kit="${kit_entry%%|*}"
    src="${kit_entry##*|}"
    mkogg_music "$M/$src" "$MUSIC/$kit.ogg"
done

# ----- Ambient loops (7) — extended Kenney sci-fi engines ----------
# Pad each source to ≥30 seconds via ffmpeg's stream_loop so the
# runtime's retrigger doesn't fire too often. Then trim to exactly
# 30s and fade in/out the seams to mask the loop point.
mkambient() {
    local src="$1" dst="$2"
    # Loop in the time domain to ~30 s, fade 50 ms in/out at seam.
    ffmpeg -y -loglevel error -stream_loop 50 -i "$src" \
           -af "atrim=duration=30,afade=t=in:st=0:d=0.05,afade=t=out:st=29.95:d=0.05" \
           -ar 22050 -ac 1 -c:a libvorbis -q:a 2 "$dst"
}

mkambient "$K/kenney_sci-fi-sounds/Audio/engineCircular_000.ogg" "$SFX/ambient_foundry.ogg"
mkambient "$K/kenney_sci-fi-sounds/Audio/spaceEngine_001.ogg"    "$SFX/ambient_maintenance.ogg"
mkambient "$K/kenney_sci-fi-sounds/Audio/computerNoise_000.ogg"  "$SFX/ambient_reactor.ogg"
mkambient "$K/kenney_sci-fi-sounds/Audio/forceField_001.ogg"     "$SFX/ambient_atrium.ogg"
mkambient "$K/kenney_sci-fi-sounds/Audio/spaceEngineLow_000.ogg" "$SFX/ambient_exterior.ogg"
mkambient "$K/kenney_sci-fi-sounds/Audio/forceField_002.ogg"     "$SFX/ambient_aurora.ogg"
mkambient "$K/kenney_sci-fi-sounds/Audio/spaceEngineLow_002.ogg" "$SFX/ambient_citadel.ogg"

echo "audio_source_map: 47 SFX + 1 servo + 7 music + 7 ambient = 62 entries materialized"
