#!/bin/bash
#
# tools/audio_normalize/normalize.sh — P19 audio format pass.
#
# Idempotent re-encode of every sample under assets/sfx/ +
# assets/music/ to the spec format from documents/m5/09-audio.md:
#
#   - assets/sfx/*.wav        → 16-bit PCM mono 22050 Hz
#   - assets/sfx/ambient_*.ogg → Vorbis q3 mono 22050 Hz (~96 kbps)
#   - assets/music/*.ogg      → Vorbis q4 mono 22050 Hz (~128 kbps)
#
# Reads the actual format via ffprobe and skips files that already
# match the spec. The skip predicate is the entire idempotency
# guarantee: re-running the script after sourcing one new file
# only touches that file.
#
# Pass --loudness to also apply ffmpeg's loudnorm filter
# (-16 LUFS for one-shot SFX, -23 LUFS for music, -28 LUFS for
# ambient). Loudness is gated because the filter is slow and
# typically wanted as a one-shot final pass.
#
# Records every action to tools/audio_normalize/normalized.txt
# (truncated each run).

set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

LOUDNESS=0
for arg in "$@"; do
    case "$arg" in
        --loudness) LOUDNESS=1 ;;
        *)          echo "unknown arg: $arg" >&2 ; exit 2 ;;
    esac
done

LOG="tools/audio_normalize/normalized.txt"
: > "$LOG"

# Returns 0 if $1 is a regular file and matches the target spec for
# its slot. Otherwise non-zero (caller re-encodes).
#
# ffprobe's CSV-of-stream entries output is ordered by ffmpeg's
# internal struct layout, not the request order. Probe each field
# individually so the comparison is unambiguous.
probe_field() {
    ffprobe -v error -select_streams a:0 \
            -show_entries "stream=$1" -of "default=nw=1:nk=1" \
            "$2" 2>/dev/null
}

check_wav() {
    local f="$1"
    [ -f "$f" ] || return 1
    [ "$(probe_field sample_rate "$f")" = "22050" ] || return 1
    [ "$(probe_field channels "$f")" = "1" ]        || return 1
    [ "$(probe_field sample_fmt "$f")" = "s16" ]    || return 1
    return 0
}

check_ogg() {
    local f="$1"
    [ -f "$f" ] || return 1
    [ "$(probe_field codec_name "$f")" = "vorbis" ] || return 1
    [ "$(probe_field sample_rate "$f")" = "22050" ] || return 1
    [ "$(probe_field channels "$f")" = "1" ]        || return 1
    return 0
}

# Normalize one WAV in place (16-bit PCM mono 22050 Hz).
#
# When --loudness, **all WAVs get peak normalization to -1.5 dB
# true peak**, NOT LUFS-based loudnorm. Reason: every WAV in
# assets/sfx/ is a transient game cue (weapon fire, hit, footstep,
# pickup chime, UI click) — none is sustained content. ffmpeg's
# `loudnorm` filter measures *integrated* loudness over the whole
# sample including the silent tail after a brief impact; for a
# 0.12 s footstep the integration window averages near-zero, so
# loudnorm thinks it's quiet and pulls the gain DOWN to hit the
# -16 LUFS target. That dropped the footstep peak from -0.9 dB
# (source) to -25.5 dB (post-loudnorm) — inaudible in real play.
#
# Peak normalization keeps the transient energy that makes impacts
# feel like impacts. Music + ambient OGGs still use loudnorm (long
# sustained signal — integrated loudness is meaningful there).
normalize_wav() {
    local f="$1"
    if check_wav "$f" && [ "$LOUDNESS" = "0" ]; then
        return 0
    fi
    local tmp="${f}.tmp.wav"
    local filter=""
    if [ "$LOUDNESS" = "1" ]; then
        # Detect max amplitude, compute gain to bring peak to
        # -1.5 dB, apply via the volume filter.
        local max_db
        max_db=$(ffmpeg -hide_banner -i "$f" -af volumedetect \
                        -f null /dev/null 2>&1 \
                        | grep max_volume \
                        | awk '{print $5}')
        if [ -n "$max_db" ]; then
            local gain_db
            gain_db=$(awk -v m="$max_db" 'BEGIN { printf "%.2f", -1.5 - m }')
            filter="-af volume=${gain_db}dB"
        fi
    fi
    # shellcheck disable=SC2086
    ffmpeg -y -loglevel error -i "$f" $filter \
           -ar 22050 -ac 1 -sample_fmt s16 "$tmp"
    mv "$tmp" "$f"
    echo "$f" >> "$LOG"
}

# Normalize one OGG-Vorbis in place. q3 by default, q4 for music,
# q2 for ambient (smaller, less critical). LUFS target varies.
normalize_ogg() {
    local f="$1"
    local quality="$2"   # vorbis -q:a value
    local lufs="$3"      # LUFS target ("" to skip)
    if check_ogg "$f" && [ "$LOUDNESS" = "0" ]; then
        return 0
    fi
    local tmp="${f}.tmp.ogg"
    local filter=""
    if [ "$LOUDNESS" = "1" ] && [ -n "$lufs" ]; then
        filter="-af loudnorm=I=${lufs}:TP=-1.5:LRA=11"
    fi
    # shellcheck disable=SC2086
    ffmpeg -y -loglevel error -i "$f" $filter \
           -ar 22050 -ac 1 -c:a libvorbis -q:a "$quality" "$tmp"
    mv "$tmp" "$f"
    echo "$f" >> "$LOG"
}

touched=0

# SFX WAVs (one-shot)
shopt -s nullglob
for f in assets/sfx/*.wav; do
    normalize_wav "$f"
    touched=$((touched + 1))
done

# Ambient OGGs (looping background bed)
for f in assets/sfx/ambient_*.ogg; do
    normalize_ogg "$f" 2 -28
    touched=$((touched + 1))
done

# Music OGGs
for f in assets/music/*.ogg; do
    normalize_ogg "$f" 4 -23
    touched=$((touched + 1))
done

# Count actual re-encodes (lines written to $LOG).
encoded=$(wc -l < "$LOG" | tr -d ' ')
echo "audio_normalize: scanned $touched files, re-encoded $encoded (see $LOG)"
if [ "$LOUDNESS" = "1" ]; then
    echo "audio_normalize: loudness pass applied (-16/-23/-28 LUFS SFX/music/ambient)"
fi
