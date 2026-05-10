#pragma once

#include "math.h"

#include <stdbool.h>

/*
 * audio — sample loading, alias pool, listener-relative pan/volume,
 *         ducking, music streaming, ambient loops, mech servo loop.
 *
 * raylib's miniaudio backend handles the mix on its own thread; we
 * never call any of the raylib audio API from inside the audio
 * callback. Every entry point here runs on the main thread.
 *
 * A fresh checkout has no `assets/sfx/`, `assets/music/`, or
 * `assets/ambient/` files on disk — `audio_init` walks the manifest
 * regardless, logs INFO for each missing file, and leaves that
 * SfxId's `alias_count == 0`. `audio_play_at` / `audio_play_global`
 * silently no-op for those ids, so the build runs without audio
 * assets while the asset-generation pipeline (P15+) catches up.
 *
 * Per `documents/m5/09-audio.md`, the listener is the local mech's
 * chest. For dead/absent local players we fall back to the last
 * known listener position so far-away cues still attenuate
 * sensibly (rather than collapsing to (0,0)).
 */

struct Game;
struct World;

typedef enum {
    SFX_NONE = 0,

    /* Weapon fire (one source per weapon, 3-8 aliases each). Indexed
     * by `audio_sfx_for_weapon(WeaponId)`. */
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
    SFX_EXPLOSION_LARGE,           /* Mass Driver direct + ≥100 dmg AOE */
    SFX_EXPLOSION_MEDIUM,          /* Frag Grenade, Plasma Cannon */
    SFX_EXPLOSION_SMALL,           /* Micro-rockets */

    /* Movement */
    SFX_FOOTSTEP_METAL,
    SFX_FOOTSTEP_CONCRETE,
    SFX_FOOTSTEP_ICE,
    SFX_JET_PULSE,                 /* per-tick "puff" while jet held */
    SFX_JET_BOOST,                 /* Burst-jet dump */
    SFX_LANDING_HARD,
    SFX_LANDING_SOFT,
    /* MECH_SERVO_LOOP is loaded separately as the modulated `g_servo`
     * Sound — not registered as an SfxEntry because it never plays via
     * audio_play_at. Kept here as documentation of the slot. */

    /* Pickups */
    SFX_PICKUP_HEALTH,
    SFX_PICKUP_AMMO,
    SFX_PICKUP_ARMOR,
    SFX_PICKUP_WEAPON,
    SFX_PICKUP_POWERUP,
    SFX_PICKUP_JET_FUEL,
    SFX_PICKUP_RESPAWN,            /* high-tier respawn cue */

    /* Grapple */
    SFX_GRAPPLE_FIRE,
    SFX_GRAPPLE_HIT,
    SFX_GRAPPLE_RELEASE,
    SFX_GRAPPLE_PULL_LOOP,

    /* Flag */
    SFX_FLAG_PICKUP,
    SFX_FLAG_DROP,
    SFX_FLAG_RETURN,
    SFX_FLAG_CAPTURE,              /* global, played for everyone */

    /* UI */
    SFX_UI_HOVER,
    SFX_UI_CLICK,
    SFX_UI_TOGGLE,

    /* Death */
    SFX_KILL_FANFARE,              /* local-mech got a kill */
    SFX_DEATH_GRUNT,               /* local-mech died */

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

/* Init / shutdown — call after / before platform_init / platform_shutdown
 * so raylib's audio device is alive when LoadSound runs and still alive
 * when UnloadSound runs. `audio_init` stashes `g` so `audio_play_at`
 * can read the local mech's listener position later without the caller
 * threading the Game through. */
bool audio_init(struct Game *g);
void audio_shutdown(void);

/* Per-frame: tick ducking, advance music stream, ambient retrigger,
 * decay servo-loop volume target. Call once per render frame. */
void audio_step(struct World *w, float dt);

/* Play SFX at a world position. Volume + pan are computed from
 * (source - listener) where listener = local mech's chest. Falls back
 * to the last known listener position when the local mech is dead /
 * absent so distant cues still attenuate. */
void audio_play_at(SfxId id, Vec2 source);

/* Play SFX globally — no spatialization, full volume. Used for events
 * where source position is irrelevant (UI clicks, kill fanfare, CTF
 * capture). The bus is picked per-id from the internal manifest
 * (UI sounds use AUDIO_BUS_UI; everything else uses AUDIO_BUS_SFX). */
void audio_play_global(SfxId id);

/* Volume control. `v` is linear gain in [0, 1]. Master + per-bus
 * gains multiply through `bus_resolved` on every play call. */
void audio_set_bus_volume(AudioBusId bus, float v);

/* Ducking. `factor` ∈ [0, 1] is the temporary multiplier applied to
 * music + ambient buses; `seconds` is how long it takes to recover to
 * 1.0. Stacks across simultaneous calls by taking the lower (more
 * ducked) value. Used by `explosion_spawn` for big detonations. */
void audio_request_duck(float factor, float seconds);

/* Music streaming. `path` is the OGG file to stream. NULL/"" stops
 * any current track. The track loops indefinitely; per-map switches
 * are a hard cut (no crossfade). */
void audio_set_music_for_map(const char *path);
void audio_music_play(void);
void audio_music_stop(void);

/* Ambient loops. Small samples that retrigger when their playback
 * ends (raylib's Sound has no built-in looping). */
void audio_set_ambient_loop(const char *path);

/* Servo loop — modulate volume by the local mech's per-tick velocity.
 * `audio_step` calls this internally with the local mech's pelvis
 * velocity; exposed for diagnostics + tests. */
void audio_servo_update(float velocity_pxs);

/* Map a WeaponId to its weapon-fire SFX. Returns SFX_NONE for unknown
 * ids. Public so weapons.c / mech.c don't have to keep their own table
 * in sync with the SfxId enum. */
SfxId audio_sfx_for_weapon(int weapon_id);

/* Hot-reload entry point. Called from hotreload's callback when a
 * watched .wav / .ogg changes on disk. DEV_BUILD-only path; release
 * builds never trigger this. */
void audio_reload_path(const char *path);

/* Register every audio asset with the hot-reload watcher. Walks the
 * SFX manifest + servo path internally. Music + ambient files aren't
 * registered at startup because their paths only become known after
 * `audio_set_music_for_map` / `audio_set_ambient_loop` lands a level
 * — those reload via the per-map switch (cheap stop+load), which is
 * good enough for M5 iteration. */
void audio_register_hotreload(void);
