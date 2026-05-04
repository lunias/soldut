# P14 — Audio module

## What this prompt does

Implements `src/audio.{c,h}` from scratch. Sample loading + alias pool + listener-relative pan/volume + ducking + per-map music streaming + ambient loops + servo loop modulated by velocity. Wires audio cues into all the call sites stubbed by P05/P06/P07.

## Required reading

1. `CLAUDE.md`
2. `documents/06-rendering-audio.md` §"Audio"
3. **`documents/m5/09-audio.md`** — the spec
4. `src/particle.c`, `src/projectile.c`, `src/mech.c` — call sites that need audio cues
5. `src/main.c` — for audio init + per-frame `audio_step` integration

## Concrete tasks

### Task 1 — `src/audio.{h,c}`

Public API per `documents/m5/09-audio.md` §"Module shape":

```c
typedef enum SfxId { /* ~50 entries per the doc */ } SfxId;
typedef enum AudioBusId { AUDIO_BUS_MASTER, AUDIO_BUS_SFX, AUDIO_BUS_MUSIC, AUDIO_BUS_AMBIENT, AUDIO_BUS_UI, AUDIO_BUS_COUNT } AudioBusId;

bool audio_init(struct Game *g);
void audio_shutdown(void);
void audio_step(struct World *w, float dt);
void audio_play_at(SfxId id, Vec2 source);
void audio_play_global(SfxId id);
void audio_set_bus_volume(AudioBusId bus, float v);
void audio_request_duck(float factor, float seconds);
void audio_set_music_for_map(const char *path);
void audio_music_play(void);
void audio_music_stop(void);
void audio_set_ambient_loop(const char *path);
void audio_servo_update(float velocity_pxs);
```

### Task 2 — SFX manifest + alias pool

Per `documents/m5/09-audio.md` §"Sample loading & alias pool":

Static manifest table: `(SfxId, path, alias_count, default_volume)`. ~50 entries. Each entry: 3-8 aliases per the per-weapon table.

`audio_init` walks the manifest, loads each `Sound`, generates aliases via `LoadSoundAlias`, stores in `g_sfx[SFX_COUNT]`.

### Task 3 — `audio_play_at` with attenuation + pan

Per `documents/m5/09-audio.md`:

```c
Vec2 listener = audio_listener_pos();
float dx = source.x - listener.x;
float dy = source.y - listener.y;
float dist = sqrtf(dx*dx + dy*dy);
float vol_dist = 1.0f - clampf((dist - 200) / 1300, 0, 1);
float pan = clampf(dx / 800, -1, 1);
SetSoundPan(s, 0.5f * (1 - pan));
SetSoundVolume(s, sound_volume * vol_dist * bus_resolved(SFX));
PlaySound(s);
```

Round-robin alias rotation.

### Task 4 — Ducking

Per `documents/m5/09-audio.md` §"Ducking":

```c
typedef struct { float gain; float duck_target; float duck_decay_per_sec; } AudioBus;
static AudioBus g_buses[AUDIO_BUS_COUNT];
```

`audio_request_duck(factor, seconds)` lowers music + ambient bus duck_target. `audio_step` lerps it back over `seconds`. Stacks via min().

Wire `audio_request_duck(0.5f, 0.30f)` into `explosion_spawn` for big events (>100 dmg).

### Task 5 — Servo loop

Per `documents/m5/09-audio.md` §"Servo loop modulation":

`g_servo` is a Sound. Re-trigger if not playing. Volume modulates with local mech velocity:

```c
float t = clamp(velocity_pxs / 280.0f, 0, 1);
g_servo_target_vol = t * 0.7f;
g_servo_current_vol += (target - current) * 0.15f;
SetSoundVolume(g_servo, current * bus_resolved(SFX));
```

Local mech only — don't loop for remote mechs.

### Task 6 — Music streaming + ambient loops

Per `documents/m5/09-audio.md` §"Music streaming" + §"Ambient loops":

`audio_set_music_for_map(path)` loads via `LoadMusicStream`, sets looping. `audio_step` calls `UpdateMusicStream`.

Ambient: small looped Sound. Re-trigger if not playing.

### Task 7 — Wire all audio call sites

Per `documents/m5/09-audio.md` §"Wiring up the audio events" — ~30 call sites across:

- `weapons.c::weapons_fire_*` — `audio_play_at(SFX_WPN_*, muzzle)`
- `weapons.c` / `projectile.c` (hit_mech path) — `audio_play_at(SFX_HIT_FLESH, hit_pos)`
- `weapons.c` / `projectile.c` (hit_wall path) — `audio_play_at(SFX_HIT_CONCRETE, end_pos)`
- `projectile.c::explosion_spawn` — `audio_play_at(SFX_EXPLOSION_*, pos)` + `audio_request_duck`
- `mech.c::mech_step_drive` (foot stance transition) — footsteps
- `mech.c::apply_jet_force` — jet pulse (rate-limited)
- `pickup.c::pickup_step` — grab + respawn cues (P05's stubs)
- `weapons.c::grapple_*` — grapple cues (P06's stubs)
- `ctf.c::ctf_*` — flag cues (P07's stubs)
- `ui.c::ui_button` — UI hover/click
- `mech.c::mech_kill` — kill fanfare (when killer is local)

Replace the no-op stubs from P05/P06/P07 with real `audio_play_at` calls.

### Task 8 — Per-map music switch

In `start_round` (after `map_build`), call `audio_set_music_for_map(level.meta.music_path_resolved)`. Hard-cut between map tracks (no crossfade).

## Done when

- `make` builds clean.
- A round of FFA on Foundry plays back: footsteps, weapon fire, hits, explosions, music. Mass Driver firing audibly ducks the music.
- Per-map music switches at round transitions.
- Servo loop modulates with movement (silent stationary, audible running).
- Hot reload of a `.wav` picks up the new sample without restart.
- Audio total RAM usage under 30 MB.

## Out of scope

- Crossfade between tracks (M6).
- 3D HRTF / true binaural pan (out of scope).
- Per-chassis servo loop variations.

## How to verify

```bash
make
./soldut --host 23073    # play and listen
```

## Close-out

1. Update `CURRENT_STATE.md`: audio module in.
2. Update `TRADE_OFFS.md`:
   - **Add** "No crossfade between map tracks" (pre-disclosed).
   - **Add** "No 3D HRTF / binaural pan" (pre-disclosed).
   - **Add** "Servo loop only for the local mech" (pre-disclosed).
3. Don't commit unless explicitly asked.

## Common pitfalls

- **`LoadSoundAlias` shares the wave data** — don't call `UnloadSound` on an alias whose source is still loaded; call on the source only at shutdown.
- **`PlaySound` retriggers**: silent restart is the right behavior for the round-robin pattern.
- **`UpdateMusicStream` per frame**: missing it = audio underrun = music stutter. Don't gate it.
- **`InitAudioDevice` is idempotent under raylib**, but call it once per process.
- **Servo loop volume jitter** from fast velocity changes: the lerp factor `0.15` smooths it; don't tune to zero.
- **Audio thread vs main thread**: never call `LoadTexture`/`LoadSound` from audio callbacks. We don't (raylib's `Sound` API is thread-safe at our use level), but be aware.
- **Mass Driver firing kicks duck**: trigger from `explosion_spawn`, not from `weapons_fire_hitscan` (since the explosion is the loud event).
- **Asset paths from META string-table**: resolve through level's STRT, not literal strings.
