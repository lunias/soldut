# M5 — Audio module

The audio layer that retires the silent prototype. New module `src/audio.{c,h}` ships at M5; loads samples, manages alias pools, plays sounds at world positions with attenuation + pan, runs ducking, streams music, owns ambient loops.

The systems that fire audio cues are spread across the rest of M5 — pickups ([04-pickups.md](04-pickups.md)), grapple ([05-grapple.md](05-grapple.md)), CTF ([06-ctf.md](06-ctf.md)), the existing weapon/projectile fire paths, and per-map ambient zones from [01-lvl-format.md](01-lvl-format.md). This document covers the *one module* that does the actual audio work.

## Where we start from

The M4 build is **silent**. raylib's miniaudio backend is linked in (it ships inside libraylib.a), and `InitAudioDevice()` is called by raylib's `InitWindow`, but no `Sound`/`Music`/`SoundAlias` resources are loaded and no `PlaySound` calls are made. The `assets/sfx/` directory is empty.

This is the milestone where we turn it on.

## Module shape

```c
// src/audio.h
#pragma once

#include "math.h"

typedef enum {
    SFX_NONE = 0,

    /* Weapon fire (per-weapon — one source, 5 aliases each) */
    SFX_WPN_PULSE_RIFLE,
    SFX_WPN_PLASMA_SMG,
    SFX_WPN_RIOT_CANNON,
    SFX_WPN_RAIL_CANNON,
    SFX_WPN_AUTO_CANNON,
    SFX_WPN_MASS_DRIVER,
    SFX_WPN_PLASMA_CANNON,
    SFX_WPN_MICROGUN,
    SFX_WPN_SIDEARM,
    SFX_WPN_BURST_SMG,
    SFX_WPN_FRAG_THROW,
    SFX_WPN_MICRO_ROCKETS,
    SFX_WPN_KNIFE_MELEE,
    SFX_WPN_KNIFE_THROW,

    /* Hits */
    SFX_HIT_FLESH,
    SFX_HIT_METAL,
    SFX_HIT_CONCRETE,

    /* Explosions / detonations */
    SFX_EXPLOSION_LARGE,        /* Mass Driver direct */
    SFX_EXPLOSION_MEDIUM,       /* Frag Grenade, Plasma Cannon */
    SFX_EXPLOSION_SMALL,        /* Micro-rockets */

    /* Movement */
    SFX_FOOTSTEP_METAL,
    SFX_FOOTSTEP_CONCRETE,
    SFX_FOOTSTEP_ICE,
    SFX_JET_PULSE,              /* per-tick "puff" while jet held */
    SFX_JET_BOOST,              /* Burst-jet dump */
    SFX_LANDING_HARD,
    SFX_LANDING_SOFT,
    SFX_MECH_SERVO_LOOP,        /* continuous, modulated by velocity */

    /* Pickups */
    SFX_PICKUP_HEALTH,
    SFX_PICKUP_AMMO,
    SFX_PICKUP_ARMOR,
    SFX_PICKUP_WEAPON,
    SFX_PICKUP_POWERUP,
    SFX_PICKUP_JET_FUEL,
    SFX_PICKUP_RESPAWN,         /* high-tier respawn cue */

    /* Grapple */
    SFX_GRAPPLE_FIRE,
    SFX_GRAPPLE_HIT,
    SFX_GRAPPLE_RELEASE,
    SFX_GRAPPLE_PULL_LOOP,

    /* Flag */
    SFX_FLAG_PICKUP,
    SFX_FLAG_DROP,
    SFX_FLAG_RETURN,
    SFX_FLAG_CAPTURE,           /* global, played for everyone */

    /* UI */
    SFX_UI_HOVER,
    SFX_UI_CLICK,
    SFX_UI_TOGGLE,

    /* Death */
    SFX_KILL_FANFARE,           /* local-mech got a kill */
    SFX_DEATH_GRUNT,            /* local-mech died */

    SFX_COUNT
} SfxId;

typedef enum {
    AUDIO_BUS_MASTER = 0,
    AUDIO_BUS_SFX,
    AUDIO_BUS_MUSIC,
    AUDIO_BUS_AMBIENT,
    AUDIO_BUS_UI,
    AUDIO_BUS_COUNT
} AudioBusId;

bool audio_init(struct Game *g);
void audio_shutdown(void);

/* Per-frame: tick ducking, advance music stream, advance ambient
 * loops, decay servo-loop volume targets. */
void audio_step(struct World *w, float dt);

/* Play SFX at world position. Volume + pan computed from
 * (source - listener) where listener = local mech's chest position. */
void audio_play_at(SfxId id, Vec2 source);

/* Play SFX globally — same volume/pan everywhere (CTF capture, kill
 * fanfare, UI). */
void audio_play_global(SfxId id);

/* Volume controls (0..1; SetVolume on the underlying buses). */
void audio_set_bus_volume(AudioBusId bus, float v);

/* Ducking — request a temporary attenuation of music + ambient buses
 * by `factor` (0..1) for `seconds`. Ducks decay back to 1.0 over their
 * duration. Stacks (multiple events take the strongest duck). */
void audio_request_duck(float factor, float seconds);

/* Music: load a track (streamed) for the current map. Replaces any
 * playing track. */
void audio_set_music_for_map(const char *path);
void audio_music_play(void);
void audio_music_stop(void);

/* Ambient: load a looped ambient sample for the current map. */
void audio_set_ambient_loop(const char *path);

/* Servo loop: modulate based on local mech's velocity each tick.
 * audio_step calls this internally; exposed for diagnostics. */
void audio_servo_update(float velocity_pxs);
```

## Bus / volume model

5 logical buses; each has a current volume that's the product of:

- The bus's gain setting (player-configurable in M6, but at M5 always 1.0).
- The duck multiplier (decays after a duck request).
- The master volume.

raylib's `SetSoundVolume(s, v)` sets a single volume per `Sound`. We don't have hardware-level buses — we replicate the bus structure in software:

```c
// src/audio.c — internal
typedef struct {
    float gain;
    float duck_target;       // 1.0 normal; <1.0 currently ducking; lerps back to 1.0
    float duck_decay_per_sec;
} AudioBus;

static AudioBus g_buses[AUDIO_BUS_COUNT];

static float bus_resolved(AudioBusId bus) {
    return g_buses[AUDIO_BUS_MASTER].gain *
           g_buses[bus].gain *
           g_buses[bus].duck_target;
}
```

Every `PlaySound` / `UpdateMusicStream` call multiplies the per-source volume by `bus_resolved()` for the appropriate bus.

### Default mix

| Bus | Default gain | dB equivalent | Notes |
|---|---|---|---|
| MASTER | 1.0 | 0 | |
| SFX | 1.0 | 0 | The anchor — gunshots/explosions are loudest. |
| UI | 0.7 | -3 | Subtle; click/hover shouldn't dominate. |
| AMBIENT | 0.45 | -7 | Background bed. |
| MUSIC | 0.30 | -10 | Lower than mix conventions for "FPS music"; we're frenetic, music is mood support. |

The dB column is for documentation; raylib API is linear gain. Conversions: -3 dB ≈ 0.71, -6 dB ≈ 0.5, -10 dB ≈ 0.32, -12 dB ≈ 0.25.

### Ducking

When a Mass Driver fires (or any "very loud event" — explosion bigger than 100 damage):

```c
audio_request_duck(0.5f, 0.30f);   // 50% music+ambient for 300 ms
```

The duck decays linearly back to 1.0 over the requested duration. Stacking: two requests overlap by taking the lower (more ducked) value.

```c
// src/audio.c::audio_step
void audio_step(World *w, float dt) {
    for (int i = 0; i < AUDIO_BUS_COUNT; ++i) {
        AudioBus *b = &g_buses[i];
        if (b->duck_target < 1.0f) {
            float recover = dt / b->duck_decay_per_sec;   // duration as a rate
            b->duck_target += recover;
            if (b->duck_target > 1.0f) b->duck_target = 1.0f;
        }
    }
    // Music streaming
    audio_music_step(dt);
    // Ambient loops continue automatically (raylib's PlaySound + IsSoundPlaying)
    // Servo loop update from local mech's velocity
    if (w->local_mech_id >= 0) {
        Mech *m = &w->mechs[w->local_mech_id];
        int pelv = m->particle_base + PART_PELVIS;
        float dx = w->particles.pos_x[pelv] - w->particles.prev_x[pelv];
        float dy = w->particles.pos_y[pelv] - w->particles.prev_y[pelv];
        float vel = sqrtf(dx*dx + dy*dy) / dt;
        audio_servo_update(vel);
    }
}
```

Per-frame cost: ~0.05 ms. Negligible.

## Sample loading & alias pool

Each SFX has one source `Sound` and N aliases via `LoadSoundAlias`. Aliases play overlapping without copying wave data. Per the research:

- **3 aliases**: minimum to break the same-sample-loop pattern. Used for slow-fire weapons (Mass Driver, Rail Cannon, Sidearm).
- **5 aliases**: sweet spot for fast-fire (Pulse Rifle, Auto-Cannon, Microgun, Plasma SMG).
- **8 aliases**: only for *very* rapid (Microgun's 25 ms cycle = 40/s). Otherwise overkill.

```c
// src/audio.c
#define MAX_ALIASES_PER_SFX 8

typedef struct {
    Sound source;
    Sound aliases[MAX_ALIASES_PER_SFX];
    int   alias_count;
    int   next_alias;          // round-robin
} SfxEntry;

static SfxEntry g_sfx[SFX_COUNT];

bool audio_init(struct Game *g) {
    InitAudioDevice();   /* raylib already does this; idempotent. */

    // Per-SFX manifest:
    //   id, path, alias_count, default_volume, default_pitch
    static const struct {
        SfxId id;
        const char *path;
        int   alias_count;
        float volume;
    } manifest[] = {
        { SFX_WPN_PULSE_RIFLE,   "assets/sfx/pulse_rifle.wav",   5, 0.85f },
        { SFX_WPN_PLASMA_SMG,    "assets/sfx/plasma_smg.wav",    5, 0.80f },
        { SFX_WPN_RIOT_CANNON,   "assets/sfx/riot_cannon.wav",   3, 1.00f },
        { SFX_WPN_RAIL_CANNON,   "assets/sfx/rail_cannon.wav",   3, 1.00f },
        { SFX_WPN_AUTO_CANNON,   "assets/sfx/auto_cannon.wav",   5, 0.90f },
        { SFX_WPN_MASS_DRIVER,   "assets/sfx/mass_driver.wav",   3, 1.00f },
        { SFX_WPN_PLASMA_CANNON, "assets/sfx/plasma_cannon.wav", 3, 0.90f },
        { SFX_WPN_MICROGUN,      "assets/sfx/microgun.wav",      8, 0.70f },
        { SFX_WPN_SIDEARM,       "assets/sfx/sidearm.wav",       3, 0.80f },
        { SFX_WPN_BURST_SMG,     "assets/sfx/burst_smg.wav",     3, 0.85f },
        // ... rest of the manifest
    };

    int n = (int)(sizeof(manifest) / sizeof(manifest[0]));
    for (int i = 0; i < n; ++i) {
        SfxEntry *e = &g_sfx[manifest[i].id];
        e->source = LoadSound(manifest[i].path);
        if (!e->source.frameCount) {
            LOG_W("audio: %s missing", manifest[i].path);
            continue;
        }
        e->alias_count = manifest[i].alias_count;
        for (int k = 0; k < e->alias_count; ++k) {
            e->aliases[k] = LoadSoundAlias(e->source);
            SetSoundVolume(e->aliases[k], manifest[i].volume);
        }
    }
    return true;
}
```

raylib's `LoadSoundAlias` doesn't copy the wave data; it shares the underlying buffer. 100 simultaneously-playing alias instances cost only the per-instance playback state (a few hundred bytes each). Memory-cheap.

## audio_play_at

```c
void audio_play_at(SfxId id, Vec2 source) {
    SfxEntry *e = &g_sfx[id];
    if (e->alias_count == 0) return;

    // Compute attenuation + pan from the local mech's chest position.
    Vec2 listener = audio_listener_pos();
    float dx = source.x - listener.x;
    float dy = source.y - listener.y;
    float dist = sqrtf(dx*dx + dy*dy);

    // Attenuation: full volume within 200 px, linear falloff to zero
    // at 1500 px.
    float vol_dist = 1.0f - clampf((dist - 200.0f) / 1300.0f, 0.0f, 1.0f);

    // Pan: full pan past 800 px.
    float pan = clampf(dx / 800.0f, -1.0f, 1.0f);

    Sound s = e->aliases[e->next_alias];
    e->next_alias = (e->next_alias + 1) % e->alias_count;

    // raylib's SetSoundPan uses (1.0 = left only, 0.0 = right only); we
    // map our [-1, 1] to that.
    SetSoundPan(s, 0.5f * (1.0f - pan));
    float bus_v = bus_resolved(AUDIO_BUS_SFX);
    SetSoundVolume(s, sound_volume_for_id(id) * vol_dist * bus_v);
    PlaySound(s);
}
```

Round-robin alias rotation. raylib's `PlaySound` will silently restart an already-playing alias — that's by design (the rotation guarantees we hit a "free" alias most of the time, and even on collision the new sound just retriggers).

## Servo loop modulation

The mech servo loop is a continuous sound that fades in when the local mech is moving fast and out when stopped. We **don't loop** mech servos for *remote* mechs — only for the local one, both because (a) bandwidth is irrelevant, audio is local; (b) authenticity favors a single intimate loop attached to the player, not 32 simultaneous quiet hums; (c) it's cheaper.

```c
// src/audio.c
static Sound g_servo;
static float g_servo_target_vol = 0.0f;
static float g_servo_current_vol = 0.0f;
static bool  g_servo_playing = false;

void audio_init_servo(void) {
    g_servo = LoadSound("assets/sfx/mech_servo_loop.wav");
    SetSoundVolume(g_servo, 0.0f);
    PlaySound(g_servo);
    g_servo_playing = true;
    // raylib doesn't have built-in looping for Sound; we restart it
    // each tick if it's not playing.
}

void audio_servo_update(float velocity_pxs) {
    // Target: 0 when stationary, 0.7 at full sprint speed (280 px/s).
    float t = clampf(velocity_pxs / 280.0f, 0.0f, 1.0f);
    g_servo_target_vol = t * 0.7f;
    // Lerp toward target.
    g_servo_current_vol += (g_servo_target_vol - g_servo_current_vol) * 0.15f;
    SetSoundVolume(g_servo, g_servo_current_vol *
                            bus_resolved(AUDIO_BUS_SFX));
    // Re-trigger if not playing (raylib's Sound doesn't loop natively).
    if (!IsSoundPlaying(g_servo)) PlaySound(g_servo);
}
```

The 280 px/s threshold is the chassis baseline run speed. Heavier chassis run slower → quieter servo. Implicit feedback: "the slow chassis is quieter," which is the right physical intuition.

For continuously-looping audio with proper handling, raylib also has `Music` (streamed). For tiny loops we use `Sound` with re-trigger; less GPU/CPU than a streaming music slot for a 1-second sample.

## Footsteps

Per-step, on the foot's gait phase. The `mech_step_drive` already detects gait phase for the run animation; we hook the SFX trigger:

```c
// src/mech.c::mech_step_drive — when foot transitions from "swing" to "stance"
if (foot_left_just_planted) {
    audio_play_at(footstep_sfx_for_surface(left_foot_pos),
                  particle_pos(&w->particles, m->particle_base + PART_L_FOOT));
}
```

`footstep_sfx_for_surface` queries the level: SOLID concrete → `SFX_FOOTSTEP_CONCRETE`, SOLID metal (specific tile id range) → `SFX_FOOTSTEP_METAL`, ICE → `SFX_FOOTSTEP_ICE`. About 30 lines.

Footsteps fire per-foot on stance transitions. Two players running side-by-side overlap their footsteps naturally; with 5 alias slots per surface that's up to 25 simultaneous footsteps before cycling — comfortable for crowded play.

## Music streaming

raylib's `Music` is a streaming wrapper around miniaudio's decoder. We use it for the per-map music track:

```c
static Music g_music;
static bool  g_music_loaded = false;

void audio_set_music_for_map(const char *path) {
    if (g_music_loaded) {
        UnloadMusicStream(g_music);
        g_music_loaded = false;
    }
    if (!path || !*path) return;
    g_music = LoadMusicStream(path);
    g_music.looping = true;
    g_music_loaded = true;
}

void audio_music_play(void) {
    if (g_music_loaded) PlayMusicStream(g_music);
}

void audio_music_stop(void) {
    if (g_music_loaded) StopMusicStream(g_music);
}

void audio_music_step(float dt) {
    if (!g_music_loaded) return;
    UpdateMusicStream(g_music);     // decode the next chunk
    SetMusicVolume(g_music, bus_resolved(AUDIO_BUS_MUSIC));
}
```

Memory: 1 MB streaming buffer (per [10-performance-budget.md](../10-performance-budget.md)). raylib decodes ~half-second chunks at a time; the streaming thread (miniaudio's) handles the actual I/O.

`UpdateMusicStream` MUST be called every frame; if we miss it, the buffer underruns and the music skips. The audio_step loop handles it.

### Per-map music

Each map's `LvlMeta.music_str_idx` resolves to a path like `assets/music/aurora.ogg`. On round start (when the level loads), we call `audio_set_music_for_map(path)` and `audio_music_play()`.

8 maps × ~2 MB OGG-Vorbis at 96 kbps Q2 stereo (per the research) = ~16 MB on disk. RAM usage during play: 1 MB (streaming buffer).

We do **not** crossfade between map tracks at round transitions — a hard cut is fine. Crossfade doubles the streaming cost (two simultaneous decoders) for a polish feature we don't need.

### Ambient loops

Per-map ambient (wind, machinery thrum, distant rumble) is a smaller looped sample loaded into RAM:

```c
static Sound g_ambient;
static bool  g_ambient_loaded = false;

void audio_set_ambient_loop(const char *path) {
    if (g_ambient_loaded) {
        UnloadSound(g_ambient);
        g_ambient_loaded = false;
    }
    if (!path || !*path) return;
    g_ambient = LoadSound(path);
    SetSoundVolume(g_ambient, bus_resolved(AUDIO_BUS_AMBIENT));
    PlaySound(g_ambient);   // we re-trigger if it stops
    g_ambient_loaded = true;
}

void audio_ambient_step(float dt) {
    (void)dt;
    if (!g_ambient_loaded) return;
    if (!IsSoundPlaying(g_ambient)) PlaySound(g_ambient);
    SetSoundVolume(g_ambient, bus_resolved(AUDIO_BUS_AMBIENT));
}
```

8 ambient samples × ~0.5 MB each (30 s mono OGG at 64 kbps) = ~4 MB.

## SFX manifest

```
assets/sfx/
├── pulse_rifle.wav            16-bit mono 22 kHz, ~150 KB
├── plasma_smg.wav             ~140 KB
├── riot_cannon.wav            ~180 KB
├── rail_cannon.wav            ~200 KB
├── auto_cannon.wav            ~140 KB
├── mass_driver.wav            ~250 KB    (loud, low-end)
├── plasma_cannon.wav          ~180 KB
├── microgun.wav               ~120 KB
├── sidearm.wav                ~80 KB
├── burst_smg.wav              ~120 KB
├── frag_throw.wav             ~80 KB
├── micro_rockets.wav          ~120 KB
├── knife_melee.wav            ~80 KB
├── knife_throw.wav            ~80 KB
├── hit_flesh.wav              ~80 KB
├── hit_metal.wav              ~80 KB
├── hit_concrete.wav           ~80 KB
├── explosion_large.wav        ~250 KB    (Mass Driver)
├── explosion_medium.wav       ~200 KB    (Frag, Plasma Cannon)
├── explosion_small.wav        ~150 KB    (Micro-rockets)
├── footstep_metal_*.wav       ~50 KB × 4 variants
├── footstep_concrete_*.wav    ~50 KB × 4
├── footstep_ice_*.wav         ~50 KB × 4
├── jet_pulse.wav              ~80 KB
├── jet_boost.wav              ~150 KB
├── landing_hard.wav           ~120 KB
├── landing_soft.wav           ~80 KB
├── mech_servo_loop.wav        ~150 KB    (1.5 s, looping)
├── pickup_health.wav          ~80 KB
├── pickup_ammo.wav            ~80 KB
├── pickup_armor.wav           ~80 KB
├── pickup_weapon.wav          ~80 KB
├── pickup_powerup.wav         ~120 KB
├── pickup_jet_fuel.wav        ~80 KB
├── pickup_respawn.wav         ~120 KB
├── grapple_fire.wav           ~80 KB
├── grapple_hit.wav            ~80 KB
├── grapple_release.wav        ~80 KB
├── grapple_pull_loop.wav      ~120 KB    (1 s, looping)
├── flag_pickup.wav            ~80 KB
├── flag_drop.wav              ~80 KB
├── flag_return.wav            ~120 KB
├── flag_capture.wav           ~250 KB    (loud, played for everyone)
├── ui_hover.wav               ~30 KB
├── ui_click.wav               ~30 KB
├── ui_toggle.wav              ~50 KB
├── kill_fanfare.wav           ~120 KB    (local-mech-only positive cue)
├── death_grunt.wav            ~120 KB    (local-mech-only)
└── ambient_<kit>.ogg          ~500 KB × 8 kits

Music:
├── music/
│   ├── foundry.ogg            ~2 MB
│   ├── slipstream.ogg
│   ├── concourse.ogg
│   ├── reactor.ogg
│   ├── catwalk.ogg
│   ├── aurora.ogg
│   ├── crossfire.ogg
│   └── citadel.ogg
```

Total in RAM: ~6 MB (all SFX) + 4 MB (ambients) + 1 MB streaming buffer = 11 MB. Inside the 30 MB budget.

Total on disk: ~6 MB SFX + ~16 MB music + ~4 MB ambient = ~26 MB. Music dominates.

### Where to source samples

**At v1, all samples are CC0 / public-domain or Soldut-original**:

- [freesound.org](https://freesound.org) — CC0 search filter.
- [opengameart.org](https://opengameart.org) — CC0 search filter.
- Hand-recorded by the author for one-of-a-kind sounds (e.g., the mech servo loop).

We don't ship licensed samples. The "Soldut-original" sounds get released CC0 alongside the binary so anyone can fork them.

## Wiring up the audio events

The SFX trigger points across the codebase:

| Event | Where | Notes |
|---|---|---|
| Weapon fire | `weapons.c::weapons_fire_*` | `audio_play_at(SFX_WPN_*, muzzle_pos)` |
| Bullet hit | `weapons.c` / `projectile.c` (hit_mech path) | `audio_play_at(SFX_HIT_FLESH, hit_pos)` |
| Bullet wall | `weapons.c` / `projectile.c` (hit_wall path) | `audio_play_at(SFX_HIT_CONCRETE/METAL, end_pos)` |
| Explosion | `projectile.c::explosion_spawn` | `audio_play_at(SFX_EXPLOSION_*, pos); audio_request_duck(0.5, 0.3)` |
| Footstep | `mech.c::mech_step_drive` (gait phase) | `audio_play_at(SFX_FOOTSTEP_*, foot_pos)` |
| Jet pulse | `mech.c::apply_jet_force` (per-tick while jet held) | rate-limited: every 4 ticks |
| Jet boost | `mech.c` (Burst chassis BTN_DASH) | `audio_play_at(SFX_JET_BOOST, pelvis)` |
| Pickup grab | `pickup.c::apply_pickup` | `audio_play_at(SFX_PICKUP_*, spawner_pos)` |
| Pickup respawn | `pickup.c::pickup_step` (high-tier only) | `audio_play_at(SFX_PICKUP_RESPAWN, spawner_pos)` |
| Grapple fire | `weapons.c::grapple_fire` | `audio_play_at(SFX_GRAPPLE_FIRE, hand)` |
| Grapple hit | `projectile.c::on_grapple_hit` | `audio_play_at(SFX_GRAPPLE_HIT, anchor)` |
| Grapple release | `mech.c::grapple_release` | `audio_play_at(SFX_GRAPPLE_RELEASE, pelvis)` |
| Flag pickup | `ctf.c::ctf_pickup` | `audio_play_at(SFX_FLAG_PICKUP, pos)` |
| Flag drop | `ctf.c` (death path) | `audio_play_at(SFX_FLAG_DROP, pos)` |
| Flag return | `ctf.c::ctf_return_flag` | `audio_play_at(SFX_FLAG_RETURN, home_pos)` |
| Flag capture | `ctf.c::ctf_capture` | `audio_play_global(SFX_FLAG_CAPTURE)` |
| UI hover/click | `ui.c::ui_button` | `audio_play_global(SFX_UI_*)` |
| Kill fanfare | `mech.c::mech_kill` (when killer is local) | `audio_play_global(SFX_KILL_FANFARE)` |

Adding ~30 SFX call sites across the codebase. Most are 1-line additions.

## Listener position

```c
Vec2 audio_listener_pos(World *w) {
    if (w->local_mech_id < 0) return (Vec2){0, 0};
    // Use chest position — same as the camera focus point.
    return mech_chest_pos(w, w->local_mech_id);
}
```

Computed fresh each `audio_play_at` call. For dead local players, fall back to the camera target (so spectators get plausible audio).

## Hot reload

Per [08-rendering.md](08-rendering.md) §"Hot reload of assets", the mtime watcher includes `.wav` and `.ogg` files. On change:

```c
// src/audio.c::audio_reload_path — called from hotreload.c
void audio_reload_path(const char *path) {
    SfxId id = sfx_id_from_path(path);
    if (id == SFX_NONE) return;
    SfxEntry *e = &g_sfx[id];
    int saved_alias_count = e->alias_count;
    for (int k = 0; k < e->alias_count; ++k) UnloadSound(e->aliases[k]);
    UnloadSound(e->source);
    e->source = LoadSound(path);
    e->alias_count = saved_alias_count;
    for (int k = 0; k < e->alias_count; ++k) {
        e->aliases[k] = LoadSoundAlias(e->source);
    }
}
```

For music: stop, unload, reload, restart. ~10 LOC.

For ambient: same.

Hot reload of audio is dev-only.

## Audio thread caution

raylib + miniaudio runs the audio mixer on its own thread. Per [06-rendering-audio.md](../06-rendering-audio.md): **never call raylib draw or load functions from inside an audio callback.** We don't use `SetAudioStreamCallback` for the game; raylib's mix callback handles everything.

Our `audio_*` API functions are all called from the main thread (gameplay tick or render). The audio thread reads playback state we set; we don't reach in.

## Performance budget

Per [10-performance-budget.md](../10-performance-budget.md), audio cost is implicit in the "Net poll + receive" 0.30 ms slot. Our actual measurements:

- `audio_step` per frame: <0.05 ms (bus tick + servo + music decode trigger)
- `audio_play_at` per call: <0.01 ms (compute pan/vol + raylib `PlaySound`)
- Decode work: happens on miniaudio's thread; doesn't block the main loop.

Add up across a busy frame (~30 SFX calls in heavy combat): ~0.3 ms total. Inside budget.

## Done when

- `src/audio.{c,h}` exists with the API above.
- All SFX manifest entries load successfully on a fresh checkout.
- A round of FFA on Foundry plays back: footsteps, weapon fire, hits, explosions, music. Mass Driver firing audibly ducks the music.
- Per-map music switches at round transitions (not crossfaded; hard cut).
- Servo loop modulates with movement (silent when standing, audible when running).
- Hot reload of a `.wav` file picks up the new sample without restart.
- Audio total RAM usage is under 30 MB.

## Trade-offs to log

- **No crossfade between map tracks.** Hard cut at round end. Saves the cost of a second simultaneous music decoder.
- **No 3D HRTF / true binaural panning.** Linear pan, distance attenuation. Sufficient for 2D side-view.
- **Servo loop only for the local mech**, not remote mechs. Documented above.
- **Footstep variants are per-surface, 4 each.** No per-chassis variation.
- **Music + ambient share a single ducking gain.** They don't independently react to events.
- **No per-player voice / chat audio.** Out per the design canon.
- **Sample rates are mixed (22 kHz mono SFX, 44 kHz stereo music)** — miniaudio resamples internally; no quality loss audible at our use case.
