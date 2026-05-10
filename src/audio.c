#include "audio.h"

#include "game.h"
#include "hotreload.h"
#include "log.h"
#include "math.h"
#include "mech.h"
#include "weapons.h"
#include "world.h"

#include "../third_party/raylib/src/raylib.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Constants ----------------------------------------------------- */

/* Distance attenuation: full volume within ATTEN_NEAR_PX, linear
 * falloff to zero at ATTEN_FAR_PX. Per `documents/m5/09-audio.md`
 * §"audio_play_at". */
#define ATTEN_NEAR_PX   200.0f
#define ATTEN_FAR_PX    1500.0f
#define PAN_MAX_PX      800.0f

/* Servo loop: target = clamp(vel/RUN_SPEED) * MAX. Lerp current
 * toward target by SERVO_LERP each call (which the audio_step path
 * runs once per render frame). */
#define SERVO_RUN_SPEED 280.0f
#define SERVO_MAX_VOL   0.7f
#define SERVO_LERP      0.15f

/* Per-tick → per-second conversion. Sim is fixed at 60 Hz; the
 * physics step takes the per-tick displacement (`pos - prev`) and we
 * multiply by 60 to get the per-second velocity the servo target
 * compares against. Don't try to derive this from the audio_step's
 * `dt` arg — that's the render-frame dt, not the sim-tick dt. */
#define TICK_HZ         60.0f

#define MAX_ALIASES_PER_SFX 8

/* ---- Bus state ----------------------------------------------------- */

typedef struct {
    float gain;                 /* 0..1, per-bus user volume (master always 1.0 at v1) */
    float duck_target;          /* 1.0 normal; <1.0 currently ducking */
    float duck_recover_per_sec; /* rate at which duck_target lerps back to 1.0 */
} AudioBus;

static AudioBus g_buses[AUDIO_BUS_COUNT];

static float bus_resolved(AudioBusId bus) {
    if (bus < 0 || bus >= AUDIO_BUS_COUNT) bus = AUDIO_BUS_SFX;
    return g_buses[AUDIO_BUS_MASTER].gain *
           g_buses[bus].gain *
           g_buses[bus].duck_target;
}

void audio_set_bus_volume(AudioBusId bus, float v) {
    if (bus < 0 || bus >= AUDIO_BUS_COUNT) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_buses[bus].gain = v;
}

/* ---- SFX manifest + alias pool ------------------------------------ */

typedef struct {
    Sound      source;
    Sound      aliases[MAX_ALIASES_PER_SFX];
    int        alias_count;
    int        next_alias;          /* round-robin */
    float      volume;              /* per-id gain after bus + distance */
    AudioBusId bus;                 /* SFX or UI */
    bool       loaded;              /* source.frameCount > 0 at init */
} SfxEntry;

static SfxEntry g_sfx[SFX_COUNT];

typedef struct {
    SfxId       id;
    const char *path;
    int         alias_count;
    float       volume;
    AudioBusId  bus;
} SfxManifestEntry;

/* The full manifest. Paths are resolved against the working directory
 * (the game ships `assets/` next to the binary). Missing files leave
 * the corresponding SfxEntry at `loaded = false` — `audio_play_*`
 * silently no-op for those ids. Per the spec:
 *   3 aliases — slow-fire (Mass Driver, Rail, Sidearm, Riot)
 *   5 aliases — sweet-spot fast-fire (Pulse, Plasma SMG, Auto-Cannon)
 *   8 aliases — Microgun (40 shots/sec) */
static const SfxManifestEntry g_sfx_manifest[] = {
    /* --- Weapons --- */
    { SFX_WPN_PULSE_RIFLE,   "assets/sfx/pulse_rifle.wav",   5, 0.85f, AUDIO_BUS_SFX },
    { SFX_WPN_PLASMA_SMG,    "assets/sfx/plasma_smg.wav",    5, 0.80f, AUDIO_BUS_SFX },
    { SFX_WPN_RIOT_CANNON,   "assets/sfx/riot_cannon.wav",   3, 1.00f, AUDIO_BUS_SFX },
    { SFX_WPN_RAIL_CANNON,   "assets/sfx/rail_cannon.wav",   3, 1.00f, AUDIO_BUS_SFX },
    { SFX_WPN_AUTO_CANNON,   "assets/sfx/auto_cannon.wav",   5, 0.90f, AUDIO_BUS_SFX },
    { SFX_WPN_MASS_DRIVER,   "assets/sfx/mass_driver.wav",   3, 1.00f, AUDIO_BUS_SFX },
    { SFX_WPN_PLASMA_CANNON, "assets/sfx/plasma_cannon.wav", 3, 0.90f, AUDIO_BUS_SFX },
    { SFX_WPN_MICROGUN,      "assets/sfx/microgun.wav",      8, 0.70f, AUDIO_BUS_SFX },
    { SFX_WPN_SIDEARM,       "assets/sfx/sidearm.wav",       3, 0.80f, AUDIO_BUS_SFX },
    { SFX_WPN_BURST_SMG,     "assets/sfx/burst_smg.wav",     3, 0.85f, AUDIO_BUS_SFX },
    { SFX_WPN_FRAG_THROW,    "assets/sfx/frag_throw.wav",    3, 0.80f, AUDIO_BUS_SFX },
    { SFX_WPN_MICRO_ROCKETS, "assets/sfx/micro_rockets.wav", 3, 0.85f, AUDIO_BUS_SFX },
    { SFX_WPN_KNIFE_MELEE,   "assets/sfx/knife_melee.wav",   3, 0.80f, AUDIO_BUS_SFX },
    { SFX_WPN_KNIFE_THROW,   "assets/sfx/knife_throw.wav",   3, 0.80f, AUDIO_BUS_SFX },

    /* --- Hits --- */
    { SFX_HIT_FLESH,         "assets/sfx/hit_flesh.wav",     5, 0.90f, AUDIO_BUS_SFX },
    { SFX_HIT_METAL,         "assets/sfx/hit_metal.wav",     5, 0.85f, AUDIO_BUS_SFX },
    { SFX_HIT_CONCRETE,      "assets/sfx/hit_concrete.wav",  5, 0.75f, AUDIO_BUS_SFX },

    /* --- Explosions --- */
    { SFX_EXPLOSION_LARGE,   "assets/sfx/explosion_large.wav",  3, 1.00f, AUDIO_BUS_SFX },
    { SFX_EXPLOSION_MEDIUM,  "assets/sfx/explosion_medium.wav", 3, 0.90f, AUDIO_BUS_SFX },
    { SFX_EXPLOSION_SMALL,   "assets/sfx/explosion_small.wav",  3, 0.80f, AUDIO_BUS_SFX },

    /* --- Movement --- */
    { SFX_FOOTSTEP_METAL,    "assets/sfx/footstep_metal.wav",    5, 0.55f, AUDIO_BUS_SFX },
    { SFX_FOOTSTEP_CONCRETE, "assets/sfx/footstep_concrete.wav", 5, 0.55f, AUDIO_BUS_SFX },
    { SFX_FOOTSTEP_ICE,      "assets/sfx/footstep_ice.wav",      5, 0.55f, AUDIO_BUS_SFX },
    { SFX_JET_PULSE,         "assets/sfx/jet_pulse.wav",         5, 0.55f, AUDIO_BUS_SFX },
    { SFX_JET_BOOST,         "assets/sfx/jet_boost.wav",         3, 0.80f, AUDIO_BUS_SFX },
    { SFX_LANDING_HARD,      "assets/sfx/landing_hard.wav",      3, 0.85f, AUDIO_BUS_SFX },
    { SFX_LANDING_SOFT,      "assets/sfx/landing_soft.wav",      3, 0.65f, AUDIO_BUS_SFX },

    /* --- Pickups --- */
    { SFX_PICKUP_HEALTH,     "assets/sfx/pickup_health.wav",     3, 0.80f, AUDIO_BUS_SFX },
    { SFX_PICKUP_AMMO,       "assets/sfx/pickup_ammo.wav",       3, 0.75f, AUDIO_BUS_SFX },
    { SFX_PICKUP_ARMOR,      "assets/sfx/pickup_armor.wav",      3, 0.80f, AUDIO_BUS_SFX },
    { SFX_PICKUP_WEAPON,     "assets/sfx/pickup_weapon.wav",     3, 0.85f, AUDIO_BUS_SFX },
    { SFX_PICKUP_POWERUP,    "assets/sfx/pickup_powerup.wav",    3, 0.95f, AUDIO_BUS_SFX },
    { SFX_PICKUP_JET_FUEL,   "assets/sfx/pickup_jet_fuel.wav",   3, 0.75f, AUDIO_BUS_SFX },
    { SFX_PICKUP_RESPAWN,    "assets/sfx/pickup_respawn.wav",    3, 0.65f, AUDIO_BUS_SFX },

    /* --- Grapple --- */
    { SFX_GRAPPLE_FIRE,      "assets/sfx/grapple_fire.wav",      3, 0.80f, AUDIO_BUS_SFX },
    { SFX_GRAPPLE_HIT,       "assets/sfx/grapple_hit.wav",       3, 0.80f, AUDIO_BUS_SFX },
    { SFX_GRAPPLE_RELEASE,   "assets/sfx/grapple_release.wav",   3, 0.65f, AUDIO_BUS_SFX },
    { SFX_GRAPPLE_PULL_LOOP, "assets/sfx/grapple_pull_loop.wav", 3, 0.55f, AUDIO_BUS_SFX },

    /* --- Flag --- */
    { SFX_FLAG_PICKUP,       "assets/sfx/flag_pickup.wav",       3, 0.85f, AUDIO_BUS_SFX },
    { SFX_FLAG_DROP,         "assets/sfx/flag_drop.wav",         3, 0.80f, AUDIO_BUS_SFX },
    { SFX_FLAG_RETURN,       "assets/sfx/flag_return.wav",       3, 0.85f, AUDIO_BUS_SFX },
    { SFX_FLAG_CAPTURE,      "assets/sfx/flag_capture.wav",      3, 1.00f, AUDIO_BUS_SFX },

    /* --- UI --- */
    { SFX_UI_HOVER,          "assets/sfx/ui_hover.wav",          3, 0.45f, AUDIO_BUS_UI  },
    { SFX_UI_CLICK,          "assets/sfx/ui_click.wav",          3, 0.65f, AUDIO_BUS_UI  },
    { SFX_UI_TOGGLE,         "assets/sfx/ui_toggle.wav",         3, 0.55f, AUDIO_BUS_UI  },

    /* --- Death --- */
    { SFX_KILL_FANFARE,      "assets/sfx/kill_fanfare.wav",      3, 0.75f, AUDIO_BUS_SFX },
    { SFX_DEATH_GRUNT,       "assets/sfx/death_grunt.wav",       3, 0.80f, AUDIO_BUS_SFX },
};

#define SFX_MANIFEST_COUNT  (int)(sizeof g_sfx_manifest / sizeof g_sfx_manifest[0])

/* ---- Servo / music / ambient state -------------------------------- */

#define SERVO_PATH "assets/sfx/mech_servo_loop.wav"

static Sound g_servo;
static bool  g_servo_loaded     = false;
static float g_servo_target_vol = 0.0f;
static float g_servo_current_vol = 0.0f;

static Music g_music;
static bool  g_music_loaded    = false;
static char  g_music_path[256] = {0};

static Sound g_ambient;
static bool  g_ambient_loaded    = false;
static char  g_ambient_path[256] = {0};

/* Listener cache. Updated by audio_step from the local mech's chest;
 * audio_play_at reads it. Cached across ticks so a dead local player
 * still hears world cues with reasonable spatialization (rather than
 * everything collapsing to (0,0)). */
static const Game *g_audio_game = NULL;
static Vec2        g_listener_pos = {0.0f, 0.0f};
static bool        g_listener_known = false;

/* ---- Helpers ------------------------------------------------------- */

/* `clampf` is provided by `src/math.h` (inline). */

static void load_sfx_entry(SfxEntry *e, const SfxManifestEntry *m) {
    e->source = LoadSound(m->path);
    e->volume = m->volume;
    e->bus    = m->bus;
    if (e->source.frameCount == 0) {
        LOG_I("audio: %s missing — playback for this id will no-op", m->path);
        e->loaded = false;
        return;
    }
    e->loaded = true;
    int n = m->alias_count;
    if (n < 1) n = 1;
    if (n > MAX_ALIASES_PER_SFX) n = MAX_ALIASES_PER_SFX;
    e->alias_count = n;
    for (int k = 0; k < n; ++k) {
        e->aliases[k] = LoadSoundAlias(e->source);
        SetSoundVolume(e->aliases[k], e->volume);
    }
    e->next_alias = 0;
}

static void unload_sfx_entry(SfxEntry *e) {
    if (!e->loaded) return;
    for (int k = 0; k < e->alias_count; ++k) {
        UnloadSoundAlias(e->aliases[k]);
        e->aliases[k] = (Sound){0};
    }
    UnloadSound(e->source);
    e->source       = (Sound){0};
    e->alias_count  = 0;
    e->next_alias   = 0;
    e->loaded       = false;
}

static const SfxManifestEntry *manifest_for_id(SfxId id) {
    for (int i = 0; i < SFX_MANIFEST_COUNT; ++i) {
        if (g_sfx_manifest[i].id == id) return &g_sfx_manifest[i];
    }
    return NULL;
}

static const SfxManifestEntry *manifest_for_path(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < SFX_MANIFEST_COUNT; ++i) {
        if (strcmp(g_sfx_manifest[i].path, path) == 0) return &g_sfx_manifest[i];
    }
    return NULL;
}

/* ---- Listener ----------------------------------------------------- */

static void update_listener(const World *w) {
    if (!w) return;
    int mid = w->local_mech_id;
    if (mid < 0 || mid >= w->mech_count) return;
    const Mech *m = &w->mechs[mid];
    if (!m->alive) return;            /* keep last-known listener */
    g_listener_pos   = mech_chest_pos(w, mid);
    g_listener_known = true;
}

/* ---- Init / shutdown ---------------------------------------------- */

bool audio_init(struct Game *g) {
    g_audio_game = g;

    /* raylib already calls InitAudioDevice from InitWindow on most
     * platforms; calling it again is idempotent (raylib bails on
     * already-ready). The second InitAudioDevice() call here is
     * defensive — for headless / shotmode paths where InitWindow
     * may not have armed audio. */
    if (!IsAudioDeviceReady()) InitAudioDevice();

    /* Default mix per `documents/m5/09-audio.md` §"Default mix". */
    g_buses[AUDIO_BUS_MASTER]  = (AudioBus){ .gain = 1.00f, .duck_target = 1.0f };
    g_buses[AUDIO_BUS_SFX]     = (AudioBus){ .gain = 1.00f, .duck_target = 1.0f };
    g_buses[AUDIO_BUS_UI]      = (AudioBus){ .gain = 0.70f, .duck_target = 1.0f };
    g_buses[AUDIO_BUS_AMBIENT] = (AudioBus){ .gain = 0.45f, .duck_target = 1.0f };
    g_buses[AUDIO_BUS_MUSIC]   = (AudioBus){ .gain = 0.30f, .duck_target = 1.0f };

    memset(g_sfx, 0, sizeof g_sfx);
    int loaded = 0;
    for (int i = 0; i < SFX_MANIFEST_COUNT; ++i) {
        const SfxManifestEntry *m = &g_sfx_manifest[i];
        if (m->id <= SFX_NONE || m->id >= SFX_COUNT) continue;
        load_sfx_entry(&g_sfx[m->id], m);
        if (g_sfx[m->id].loaded) ++loaded;
    }
    LOG_I("audio_init: %d/%d samples loaded (missing files no-op silently)",
          loaded, SFX_MANIFEST_COUNT);

    /* Servo loop — separately tracked because its volume is
     * continuously modulated rather than per-fire. */
    g_servo = LoadSound(SERVO_PATH);
    if (g_servo.frameCount > 0) {
        g_servo_loaded = true;
        SetSoundVolume(g_servo, 0.0f);
        /* The loop is restarted whenever IsSoundPlaying goes false in
         * audio_servo_update (raylib's Sound has no native loop). */
    } else {
        LOG_I("audio_init: %s missing — servo loop disabled", SERVO_PATH);
        g_servo_loaded = false;
    }

    g_servo_target_vol  = 0.0f;
    g_servo_current_vol = 0.0f;

    /* Music + ambient start empty until per-map paths land via
     * audio_set_music_for_map / audio_set_ambient_loop. */
    g_music_loaded   = false;
    g_ambient_loaded = false;
    g_music_path[0]   = '\0';
    g_ambient_path[0] = '\0';

    g_listener_pos   = (Vec2){0.0f, 0.0f};
    g_listener_known = false;

    return true;
}

void audio_shutdown(void) {
    /* Stop streams first so the audio thread isn't reading buffers we
     * unload underneath it. */
    if (g_music_loaded)   { StopMusicStream(g_music);  UnloadMusicStream(g_music); g_music_loaded   = false; }
    if (g_ambient_loaded) { StopSound(g_ambient);      UnloadSound(g_ambient);     g_ambient_loaded = false; }
    if (g_servo_loaded)   { StopSound(g_servo);        UnloadSound(g_servo);       g_servo_loaded   = false; }

    for (int i = 0; i < SFX_COUNT; ++i) unload_sfx_entry(&g_sfx[i]);

    g_audio_game     = NULL;
    g_listener_known = false;

    /* We do NOT call CloseAudioDevice() here — platform_shutdown owns
     * the device lifetime and closes it after every audio resource has
     * been unloaded. Calling it twice is harmless on raylib but the
     * single-owner discipline is cleaner. */
}

/* ---- Per-frame step ------------------------------------------------ */

static void duck_step(float dt) {
    for (int i = 0; i < AUDIO_BUS_COUNT; ++i) {
        AudioBus *b = &g_buses[i];
        if (b->duck_target >= 1.0f) continue;
        if (b->duck_recover_per_sec > 0.0f) {
            b->duck_target += dt / b->duck_recover_per_sec;
        } else {
            b->duck_target = 1.0f;
        }
        if (b->duck_target > 1.0f) b->duck_target = 1.0f;
    }
}

static void music_step(void) {
    if (!g_music_loaded) return;
    UpdateMusicStream(g_music);
    SetMusicVolume(g_music, bus_resolved(AUDIO_BUS_MUSIC));
}

static void ambient_step(void) {
    if (!g_ambient_loaded) return;
    if (!IsSoundPlaying(g_ambient)) PlaySound(g_ambient);
    SetSoundVolume(g_ambient, bus_resolved(AUDIO_BUS_AMBIENT));
}

void audio_step(struct World *w, float dt) {
    duck_step(dt);
    music_step();
    ambient_step();

    update_listener(w);

    /* Servo modulation from local-mech velocity. The pelvis is a
     * stable centroid; (pos - prev) at that particle is the per-tick
     * displacement, so multiplying by 60 (TICK_HZ) yields per-second
     * velocity. */
    if (w && w->local_mech_id >= 0 && w->local_mech_id < w->mech_count) {
        const Mech *m = &w->mechs[w->local_mech_id];
        if (m->alive) {
            int pelv = m->particle_base + PART_PELVIS;
            float dx = w->particles.pos_x[pelv] - w->particles.prev_x[pelv];
            float dy = w->particles.pos_y[pelv] - w->particles.prev_y[pelv];
            float vel_pxs = sqrtf(dx*dx + dy*dy) * TICK_HZ;
            audio_servo_update(vel_pxs);
            return;
        }
    }
    /* Local mech absent / dead → glide servo toward zero so the loop
     * fades out instead of holding the last value indefinitely. */
    audio_servo_update(0.0f);
}

/* ---- Play paths ---------------------------------------------------- */

void audio_play_at(SfxId id, Vec2 source) {
    if (id <= SFX_NONE || id >= SFX_COUNT) return;
    SfxEntry *e = &g_sfx[id];
    if (!e->loaded || e->alias_count == 0) return;

    Vec2 listener = g_listener_known ? g_listener_pos : source;
    float dx   = source.x - listener.x;
    float dy   = source.y - listener.y;
    float dist = sqrtf(dx*dx + dy*dy);

    /* Linear attenuation: full volume within ATTEN_NEAR_PX, zero past
     * ATTEN_FAR_PX. Sub-200 px is bone-rattling; past 1500 px the cue
     * is mostly inaudible — skip the PlaySound to avoid pumping the
     * alias rotation for a sound that won't be heard. */
    if (dist > ATTEN_FAR_PX) return;
    float vol_dist = 1.0f - clampf((dist - ATTEN_NEAR_PX) /
                                   (ATTEN_FAR_PX - ATTEN_NEAR_PX),
                                   0.0f, 1.0f);

    /* Pan: full pan past ±PAN_MAX_PX. raylib's SetSoundPan uses 1.0=L,
     * 0.5=center, 0.0=R; our internal pan is [-1,+1] (-1 = full left,
     * +1 = full right), and the mapping below converts it. */
    float pan = clampf(dx / PAN_MAX_PX, -1.0f, 1.0f);

    Sound s = e->aliases[e->next_alias];
    e->next_alias = (e->next_alias + 1) % e->alias_count;

    SetSoundPan(s, 0.5f * (1.0f - pan));
    SetSoundVolume(s, e->volume * vol_dist * bus_resolved(e->bus));
    PlaySound(s);
}

void audio_play_global(SfxId id) {
    if (id <= SFX_NONE || id >= SFX_COUNT) return;
    SfxEntry *e = &g_sfx[id];
    if (!e->loaded || e->alias_count == 0) return;

    Sound s = e->aliases[e->next_alias];
    e->next_alias = (e->next_alias + 1) % e->alias_count;

    SetSoundPan(s, 0.5f);
    SetSoundVolume(s, e->volume * bus_resolved(e->bus));
    PlaySound(s);
}

/* ---- Ducking ------------------------------------------------------- */

void audio_request_duck(float factor, float seconds) {
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    if (seconds < 0.05f) seconds = 0.05f;     /* floor; arbitrarily fast = pop */

    /* Stack across simultaneous events: take the lower (more ducked)
     * value, and the longer recovery. */
    AudioBus *m = &g_buses[AUDIO_BUS_MUSIC];
    AudioBus *a = &g_buses[AUDIO_BUS_AMBIENT];
    if (factor < m->duck_target) m->duck_target = factor;
    if (factor < a->duck_target) a->duck_target = factor;
    if (seconds > m->duck_recover_per_sec) m->duck_recover_per_sec = seconds;
    if (seconds > a->duck_recover_per_sec) a->duck_recover_per_sec = seconds;
}

/* ---- Servo loop ---------------------------------------------------- */

void audio_servo_update(float velocity_pxs) {
    if (!g_servo_loaded) return;
    float t = clampf(velocity_pxs / SERVO_RUN_SPEED, 0.0f, 1.0f);
    g_servo_target_vol = t * SERVO_MAX_VOL;
    g_servo_current_vol += (g_servo_target_vol - g_servo_current_vol) * SERVO_LERP;

    /* Below the audible-floor, keep volume at zero so we don't fight
     * the SFX bus floor. */
    if (g_servo_current_vol < 0.005f) g_servo_current_vol = 0.0f;

    SetSoundVolume(g_servo, g_servo_current_vol * bus_resolved(AUDIO_BUS_SFX));
    if (!IsSoundPlaying(g_servo) && g_servo_current_vol > 0.0f) PlaySound(g_servo);
}

/* ---- Music --------------------------------------------------------- */

void audio_set_music_for_map(const char *path) {
    /* No-op when nothing changed (cheap guard for round-loop reuse on
     * the same map). */
    if (path && path[0] && g_music_loaded &&
        strcmp(path, g_music_path) == 0)
    {
        return;
    }

    if (g_music_loaded) {
        StopMusicStream(g_music);
        UnloadMusicStream(g_music);
        g_music_loaded = false;
        g_music_path[0] = '\0';
    }
    if (!path || !*path) return;

    g_music = LoadMusicStream(path);
    if (g_music.frameCount == 0) {
        LOG_I("audio: music %s not found — silence on this map", path);
        return;
    }
    g_music.looping = true;
    g_music_loaded  = true;
    snprintf(g_music_path, sizeof g_music_path, "%s", path);
    SetMusicVolume(g_music, bus_resolved(AUDIO_BUS_MUSIC));
}

void audio_music_play(void) {
    if (!g_music_loaded) return;
    PlayMusicStream(g_music);
}

void audio_music_stop(void) {
    if (!g_music_loaded) return;
    StopMusicStream(g_music);
}

/* ---- Ambient ------------------------------------------------------- */

void audio_set_ambient_loop(const char *path) {
    if (path && path[0] && g_ambient_loaded &&
        strcmp(path, g_ambient_path) == 0)
    {
        return;
    }

    if (g_ambient_loaded) {
        StopSound(g_ambient);
        UnloadSound(g_ambient);
        g_ambient_loaded = false;
        g_ambient_path[0] = '\0';
    }
    if (!path || !*path) return;

    g_ambient = LoadSound(path);
    if (g_ambient.frameCount == 0) {
        LOG_I("audio: ambient %s not found — silent ambient on this map", path);
        return;
    }
    g_ambient_loaded = true;
    snprintf(g_ambient_path, sizeof g_ambient_path, "%s", path);
    SetSoundVolume(g_ambient, bus_resolved(AUDIO_BUS_AMBIENT));
    PlaySound(g_ambient);
}

/* ---- Weapon → SFX mapping ----------------------------------------- */

SfxId audio_sfx_for_weapon(int weapon_id) {
    switch (weapon_id) {
        case WEAPON_PULSE_RIFLE:    return SFX_WPN_PULSE_RIFLE;
        case WEAPON_PLASMA_SMG:     return SFX_WPN_PLASMA_SMG;
        case WEAPON_RIOT_CANNON:    return SFX_WPN_RIOT_CANNON;
        case WEAPON_RAIL_CANNON:    return SFX_WPN_RAIL_CANNON;
        case WEAPON_AUTO_CANNON:    return SFX_WPN_AUTO_CANNON;
        case WEAPON_MASS_DRIVER:    return SFX_WPN_MASS_DRIVER;
        case WEAPON_PLASMA_CANNON:  return SFX_WPN_PLASMA_CANNON;
        case WEAPON_MICROGUN:       return SFX_WPN_MICROGUN;
        case WEAPON_SIDEARM:        return SFX_WPN_SIDEARM;
        case WEAPON_BURST_SMG:      return SFX_WPN_BURST_SMG;
        case WEAPON_FRAG_GRENADES:  return SFX_WPN_FRAG_THROW;
        case WEAPON_MICRO_ROCKETS:  return SFX_WPN_MICRO_ROCKETS;
        case WEAPON_COMBAT_KNIFE:   return SFX_WPN_KNIFE_MELEE;
        case WEAPON_GRAPPLING_HOOK: return SFX_GRAPPLE_FIRE;
        default:                    return SFX_NONE;
    }
}

/* ---- Hot reload ---------------------------------------------------- */

void audio_reload_path(const char *path) {
    if (!path) return;

    /* SFX entry? Drop aliases + source, reload, regenerate aliases. */
    const SfxManifestEntry *m = manifest_for_path(path);
    if (m) {
        SfxEntry *e = &g_sfx[m->id];
        unload_sfx_entry(e);
        load_sfx_entry(e, m);
        LOG_I("audio: reloaded %s (%d aliases)", path, e->alias_count);
        return;
    }

    /* Servo? */
    if (strcmp(path, SERVO_PATH) == 0) {
        if (g_servo_loaded) {
            StopSound(g_servo);
            UnloadSound(g_servo);
            g_servo_loaded = false;
        }
        g_servo = LoadSound(SERVO_PATH);
        g_servo_loaded = (g_servo.frameCount > 0);
        if (g_servo_loaded) SetSoundVolume(g_servo, g_servo_current_vol *
                                           bus_resolved(AUDIO_BUS_SFX));
        LOG_I("audio: reloaded servo (%s)", g_servo_loaded ? "ok" : "missing");
        return;
    }

    /* Music? Reload + restart. */
    if (g_music_path[0] && strcmp(path, g_music_path) == 0) {
        bool was_loaded = g_music_loaded;
        if (g_music_loaded) {
            StopMusicStream(g_music);
            UnloadMusicStream(g_music);
            g_music_loaded = false;
        }
        g_music = LoadMusicStream(path);
        if (g_music.frameCount > 0) {
            g_music.looping = true;
            g_music_loaded  = true;
            SetMusicVolume(g_music, bus_resolved(AUDIO_BUS_MUSIC));
            if (was_loaded) PlayMusicStream(g_music);
        }
        LOG_I("audio: reloaded music %s (%s)", path,
              g_music_loaded ? "ok" : "missing");
        return;
    }

    /* Ambient? */
    if (g_ambient_path[0] && strcmp(path, g_ambient_path) == 0) {
        if (g_ambient_loaded) {
            StopSound(g_ambient);
            UnloadSound(g_ambient);
            g_ambient_loaded = false;
        }
        g_ambient = LoadSound(path);
        if (g_ambient.frameCount > 0) {
            g_ambient_loaded = true;
            SetSoundVolume(g_ambient, bus_resolved(AUDIO_BUS_AMBIENT));
            PlaySound(g_ambient);
        }
        LOG_I("audio: reloaded ambient %s (%s)", path,
              g_ambient_loaded ? "ok" : "missing");
        return;
    }

    LOG_I("audio: hot-reload path %s not registered as a sample", path);
}

void audio_register_hotreload(void) {
    /* Per-file registration. The SFX manifest is the source of truth;
     * servo path is separate. Music / ambient files attach their own
     * lifecycle via audio_set_music_for_map / audio_set_ambient_loop
     * (a per-map stop+reload is fast enough that watching them every
     * 250 ms is unnecessary work). */
    for (int i = 0; i < SFX_MANIFEST_COUNT; ++i) {
        hotreload_register(g_sfx_manifest[i].path, audio_reload_path);
    }
    hotreload_register(SERVO_PATH, audio_reload_path);
}
