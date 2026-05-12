# P19 — Audio assets

## What this prompt does

Fills the P14 audio runtime with actual sound files. The `src/audio.c`
manifest declares ~47 SFX paths under `assets/sfx/` plus 8 per-map
music tracks under `assets/music/` plus 7 ambient loops; **none of
these files ship yet**. Every `audio_play_at` call resolves to a
silent slot — the game looks correct but you hear nothing.

P19 closes that gap. It is a **sourcing pass**, not a code-writing
pass: scripts + manifests + verification + credits. Like P15/P16,
Claude can't generate audio interactively — the user drives the
sourcing from freesound.org / opengameart.org / Kenney + does any
Soldut-original recordings; Claude builds the tooling that makes
the sourcing repeatable and confirms the runtime accepts the result.

Resolves the TRADE_OFFS entry "SFX manifest assets aren't on disk
yet (post-P14)".

## Required reading

1. `CLAUDE.md` — project conventions.
2. `documents/01-philosophy.md` — keep it small; CC0/public-domain
   only; document everything in `assets/credits.txt`.
3. **`documents/m5/09-audio.md`** §"SFX manifest" + §"Where to source
   samples" + §"Performance budget" — the spec.
4. `src/audio.c` — the actual manifest (`g_sfx_manifest[]`, ~47
   entries) + `SERVO_PATH` + how `audio_set_music_for_map` resolves
   the music path from `level.meta.music_str_idx` via STRT.
5. `tools/cook_maps/cook_maps.c::set_meta` — confirms the per-map
   kit short names that map → music/ambient paths
   (`assets/music/<kit>.ogg`, `assets/sfx/ambient_<kit>.ogg`).
6. `documents/m5/prompts/P15-comfyui-trooper.md` — for the tooling-
   first pattern (a `tools/comfy/` analogue for audio).

## Background

P14 (2026-05-09) shipped the audio module — sample loading, alias
pool, listener-relative pan/volume, ducking, music streaming,
ambient loops, servo loop modulation, ~30 call sites wired across
the gameplay code. `audio_init` logs `WARN` per missing file but
keeps running; missing entries become silent no-ops.

The maps from P17 + P18 set `level.meta.music_str_idx` and
`ambient_loop_str_idx` to STRT entries like `"assets/music/foundry.ogg"`
and `"assets/sfx/ambient_foundry.ogg"`. The 8 maps reference 7 unique
kits (Crossfire reuses Foundry's kit):

| Map | Kit short_name | Music path | Ambient path |
|---|---|---|---|
| Foundry | `foundry` | `assets/music/foundry.ogg` | `assets/sfx/ambient_foundry.ogg` |
| Slipstream | `maintenance` | `assets/music/maintenance.ogg` | `assets/sfx/ambient_maintenance.ogg` |
| Reactor | `reactor` | `assets/music/reactor.ogg` | `assets/sfx/ambient_reactor.ogg` |
| Concourse | `atrium` | `assets/music/atrium.ogg` | `assets/sfx/ambient_atrium.ogg` |
| Catwalk | `exterior` | `assets/music/exterior.ogg` | `assets/sfx/ambient_exterior.ogg` |
| Aurora | `aurora` | `assets/music/aurora.ogg` | `assets/sfx/ambient_aurora.ogg` |
| Crossfire | `foundry` (shared) | (reuses Foundry) | (reuses Foundry) |
| Citadel | `citadel` | `assets/music/citadel.ogg` | `assets/sfx/ambient_citadel.ogg` |

Plus the servo loop (`assets/sfx/mech_servo_loop.wav`) and ~47 SFX
covering weapon fire / impact / explosion / footstep / jet /
pickup / grapple / flag / UI / kill.

**Total asset footprint targets** (from `09-audio.md` §"Performance
budget"):
- SFX:  ~6 MB on disk (47 entries × ~100 KB average)
- Music: ~16 MB on disk (8 × ~2 MB OGG)
- Ambient: ~4 MB on disk (7 × ~500 KB OGG)
- **Total: ~26 MB.** Well inside the 50 MB ship budget but the
  dominant footprint inside `assets/`.

## Concrete tasks

### Task 1 — `tools/audio_inventory/` — what's missing

Build a small C tool that walks `g_sfx_manifest` + the per-map
META + `SERVO_PATH` and reports what's on disk vs missing. Reuses
`src/audio.c`'s manifest as the source of truth so it doesn't drift.

```
tools/audio_inventory/
├── run_inventory.c          # main — prints CSV of (path, kind, missing|present|wrong_format)
└── (no other files — fold helpers inline)
```

Output to stdout + write `tools/audio_inventory/missing.txt` (one
path per line) for the sourcing workflow to consume. New Makefile
target `make audio-inventory` builds + runs.

Kinds: `SFX_WPN`, `SFX_HIT`, `SFX_EXPLOSION`, `SFX_FOOT`, `SFX_JET`,
`SFX_PICKUP`, `SFX_GRAPPLE`, `SFX_FLAG`, `SFX_UI`, `SFX_KILL`,
`SERVO`, `MUSIC`, `AMBIENT`. Useful for sourcing in batches.

### Task 2 — `tools/audio_normalize/` — format pass

After sourcing, every WAV must be **16-bit PCM, mono, 22050 Hz**
(per the spec at `09-audio.md` §"SFX manifest"). Every OGG must be
**Vorbis q3-q5 mono** (~96 kbps). raylib's `LoadSound` /
`LoadMusicStream` are tolerant but format-divergence at runtime is
a debugging pit.

Build a small bash + ffmpeg wrapper that walks `assets/sfx/*.wav`
+ `assets/music/*.ogg` + `assets/sfx/ambient_*.ogg`, checks format,
and rewrites with ffmpeg if mismatched. Writes
`tools/audio_normalize/normalized.txt` listing what was touched.

```bash
# tools/audio_normalize/normalize.sh
for f in assets/sfx/*.wav; do
    ffprobe -v error -show_entries stream=sample_rate,channels,sample_fmt \
            -of csv=p=0 "$f"
    # if not "22050,1,s16" → ffmpeg -y -i "$f" -ar 22050 -ac 1 -sample_fmt s16 "$f.tmp.wav" && mv "$f.tmp.wav" "$f"
done
```

New Makefile target `make audio-normalize`. Idempotent — second run
no-ops.

### Task 3 — sourcing workflow + shopping list

Generate a markdown shopping list per kind under
`tools/audio_inventory/SHOPPING.md`:

```markdown
## Weapons (14)
- [ ] pulse_rifle.wav — "futuristic energy rifle short burst" — freesound CC0
- [ ] plasma_smg.wav  — "plasma SMG rapid fire" — opengameart CC0
- [ ] rail_cannon.wav — "rail gun charge + crack" — freesound CC0
- ...

## Impacts (3)
- [ ] hit_flesh.wav    — "wet impact body squelch" — freesound CC0
- ...

## Music (7)
- [ ] foundry.ogg     — "industrial loop ~2 min, low-mid energy"
- [ ] maintenance.ogg — "vertical map theme, dread + jet whoosh ambience"
- ...
```

Per kind, give 1-line keyword guidance the user can paste into a
freesound.org search. The shopping list is the user's checklist for
the sourcing pass; Claude updates the checkboxes as files land.

This is the **manual phase** — the user drives the browser, picks
samples, drops them in `assets/sfx/` and `assets/music/` /
`assets/sfx/ambient_*.ogg`. Claude is not in this loop.

### Task 4 — credits + provenance

Every file the user drops in gets a row in `assets/credits.txt`
(append-only, plain text, one entry per line, format mirrors the
P15 credits convention):

```
sfx/pulse_rifle.wav         freesound.org/people/<author>/sounds/<id>   CC0   freesound search "<keyword>" 2026-05-12
music/foundry.ogg           opengameart.org/content/<slug>              CC0   author <name>
sfx/mech_servo_loop.wav     Soldut-original (Lunias)                    CC0   recorded 2026-05-12
```

Build a `tools/audio_inventory/check_credits.sh` that diffs
`assets/sfx/`+`assets/music/` against `assets/credits.txt` and
flags any file present on disk without a credits entry. New
Makefile target `make audio-credits` runs the check.

This protects the project's CC0 hygiene (per
`01-philosophy.md` §"Public domain or permissive") — a file
without a credits row is a license-risk file.

### Task 5 — loudness normalization

After sourcing + format conversion, run a loudness pass so
explosions don't clip but pickup sounds aren't drowned out.
ffmpeg-loudnorm to **-16 LUFS** for one-shot SFX, **-23 LUFS** for
music (cinema-standard), **-28 LUFS** for ambient loops:

```bash
ffmpeg -i "$IN" -af loudnorm=I=-16:TP=-1:LRA=11 -ar 22050 -ac 1 -sample_fmt s16 "$OUT"
```

Fold into `tools/audio_normalize/normalize.sh` — gated by a CLI
flag `--loudness` so the user can re-run normalization without
re-running loudness (slow). Default off for the first pass.

### Task 6 — smoke verification

After sourcing + normalization + credits, the game must boot
without `[WARN] audio: sample missing` lines. Build a small smoke
script `tests/audio_smoke.sh`:

```bash
#!/bin/bash
set -e
./soldut --shot tests/shots/audio_smoke.shot 2>&1 | tee /tmp/audio_smoke.log
if grep -q "audio:.*missing" /tmp/audio_smoke.log; then
    echo "FAIL: missing audio file(s)"
    exit 1
fi
echo "PASS: no missing audio at boot"
```

`tests/shots/audio_smoke.shot` is a minimal 10-tick run that boots,
fires once, takes a frame, exits. Doesn't need to assert anything
audible — just that the audio runtime warns clean.

New Makefile target `make test-audio-smoke`.

### Task 7 — TRADE_OFFS sweep

The post-P14 entry "SFX manifest assets aren't on disk yet
(post-P14)" gets deleted when **all** of the following are true:

- `make audio-inventory` reports zero missing entries.
- `make audio-credits` reports zero unattributed files.
- `make test-audio-smoke` passes.

If a small set of files (e.g., the 3 footstep variants per surface
× 4 = 12 footstep variants) genuinely can't be sourced and a
single-variant fallback is acceptable, document that as a new
TRADE_OFFS entry — don't pretend the gap doesn't exist.

### Task 8 — close-out

1. Update `CURRENT_STATE.md`: add P19 to the M5 Done section.
2. Update `TRADE_OFFS.md`:
   - **Delete** "SFX manifest assets aren't on disk yet (post-P14)".
   - **Add** any new entries for genuinely-deferred audio gaps
     (single-variant footsteps if multi-variant didn't ship, kit
     reuse if music tracks ended up shared, etc.).
3. Update `CLAUDE.md` — M5 status line shifts P19 from Pending to
   shipped; reflect what the M5 ship now looks like complete.
4. Update `documents/m5/prompts/README.md` — annotate P19 as
   shipped with the date.

## Done when

- `make` builds clean.
- `make audio-inventory` prints "0 missing entries" (or a tracked
  TRADE_OFFS entry exists for any explicit gap).
- `make audio-normalize` is idempotent (no-op on second run).
- `make audio-credits` prints "all assets attributed".
- `make test-audio-smoke` passes.
- A 60-second `./soldut --host` round on Foundry plays back:
  weapon fire, hits, footsteps, jet pulses, music. Mass Driver
  firing audibly ducks the music; carrying the flag triggers the
  capture fanfare (CTF round on Crossfire).
- `assets/credits.txt` has one row per audio file in `assets/sfx/`
  and `assets/music/`.
- Total `assets/sfx/` + `assets/music/` size under 30 MB.

## Out of scope

- **Crossfade between tracks** — stays deferred per the existing
  TRADE_OFFS entry.
- **3D HRTF / binaural pan** — stays deferred.
- **Multi-variant footsteps if not sourced** — single-variant
  fallback is acceptable for v1 if the variants don't easily
  source. Document as a new TRADE_OFFS entry.
- **Voice / chatter / announcer** — kept silent at v1.
- **Hot reload of music streams** — `audio_register_hotreload`
  handles SFX `.wav` paths via mtime; music streams stay static
  until restart. (Already the P14 behavior — not P19's job to
  change.)

## How to verify

```bash
make audio-inventory                  # show what's missing
# … user sources files into assets/sfx/ + assets/music/ …
make audio-normalize                  # format pass (22050/mono/PCM16, OGG-Vorbis)
make audio-normalize ARGS=--loudness  # loudness normalize (slow; second pass)
make audio-credits                    # check credits coverage
make test-audio-smoke                 # confirm no missing-file warnings
make                                  # release build
./soldut --host 23073                 # play and listen
```

For visual confirmation of the music duck:

```bash
./soldut --shot tests/shots/audio_duck.shot
# grep build/shots/audio_duck/audio_duck.log for "duck_target" / "music_vol"
```

## Common pitfalls

- **Sample format silent-divergence.** raylib's `LoadSound` accepts
  44.1 kHz stereo but the project's spec is 22.05 kHz mono. Format
  pass at task 2 is non-optional — without it some samples will
  play double-speed or off-pitch from the wrong sample rate
  assumption. ffmpeg-loudnorm at task 5 doesn't re-sample by
  itself; explicitly pass `-ar 22050 -ac 1 -sample_fmt s16`.
- **OGG-Vorbis vs OGG-Opus.** raylib's `LoadMusicStream` decodes
  Vorbis. If the user grabs an `.ogg` that's actually Opus inside,
  it'll fail to open silently. `ffprobe` the codec field; reject
  Opus and ask the user for a Vorbis source (or transcode).
- **Credits file = compliance.** Don't drop a sample in without
  appending to `assets/credits.txt`. The `make audio-credits`
  gate is the safety net; running it last (in close-out) is what
  catches mistakes.
- **Loudness pass after format pass.** If you run loudnorm on a
  44.1 kHz stereo file then convert to 22 kHz mono after, the
  loudness numbers re-shift. Always: format → loudness → final.
- **Manifest drift.** `src/audio.c::g_sfx_manifest` is the source
  of truth. If a path changes there, `make audio-inventory` will
  surface the rename as a "missing" file — that's the *correct*
  signal. Update the manifest first, then re-source / rename.
- **Music streaming buffer underrun.** `audio_step` calls
  `UpdateMusicStream` every tick — if a music file's bitrate is
  too high (>192 kbps Vorbis) the decode can stutter on slow
  hardware. Stick to q3-q5 (~96-160 kbps) per the spec.
- **Kit sharing pitfall.** Crossfire shares Foundry's kit
  (`assets/music/foundry.ogg`). Make sure the per-map music
  override at round transition correctly hard-cuts even when the
  *same* kit name is requested twice in a row (the runtime should
  no-op the load if the path is unchanged; verify the smoke test
  doesn't re-decode on every Foundry→Crossfire transition).
- **Soldut-original recordings need self-licensing.** The mech
  servo loop and any author-recorded sounds get an explicit
  `Soldut-original (<author>)` line in credits.txt with a CC0
  reference — match the P15 gostek-sheet provenance convention.
- **freesound.org rate limits.** Sourcing 47 samples in one
  sitting will trip freesound's anonymous rate limit (~150/day).
  Stagger across sessions or register a free account to lift the
  cap.

## What "shipped" looks like

When P19 is done, `make` boots a game that **sounds correct**:
weapon fire matches the on-screen muzzle flash; explosions duck
the music; footsteps modulate with mech velocity via the servo
loop; per-map music switches at round transitions; flag captures
trigger the global capture fanfare. `assets/credits.txt` has
the provenance line for every shipped sample. The
"SFX manifest assets aren't on disk yet" trade-off is gone.

P19 is the last open prompt for M5. After it ships, M5 is
complete — every system has its content, every map plays with
sound, the bake-test heatmaps tell the designer what to iterate
in M6 polish.
