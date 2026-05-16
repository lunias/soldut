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
 * runs once per render frame).
 *
 * SERVO_MAX_VOL is 0.0 at post-P19 round 4 — the servo is
 * intentionally silent. User feedback across three rounds: the
 * continuous mech-presence hum masked the per-plant footstep cues
 * and read as "the walking sound" regardless of source sample
 * (spaceEngine_000 / forceField_002 / computerNoise_001 were all
 * tried). Footsteps carry the player's gait cue; servo is
 * unnecessary. The sample stays on disk + the runtime path stays
 * wired so re-enabling is a one-line change: raise MAX_VOL to e.g.
 * 0.10 to bring a subtle hum back.
 *
 * For the competitive-listening use case (hearing enemy footsteps
 * approach), the SFX bus distance attenuation (200 px full → 1500
 * px silent in `audio_play_at`) does the spatial work; bumping the
 * footstep volume in g_sfx_manifest is what controls intelligibility
 * at mid-range, not the servo cap. */
#define SERVO_RUN_SPEED 280.0f
#define SERVO_MAX_VOL   0.00f
#define SERVO_LERP      0.30f

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

/* F2 mute — process-local flag that short-circuits bus_resolved to
 * silence all output without disturbing the per-bus gains. The
 * underlying samples + music streams continue to play (raylib's
 * mixer runs regardless); only the volume read at PlaySound /
 * SetMusicVolume / SetSoundVolume reads zero. */
static bool g_muted = false;

/* Volume overlay — armed (set to VOL_OVERLAY_LIFETIME) whenever the
 * user nudges master volume via +/- keys. Decays toward zero in
 * audio_step. While > 0, audio_draw_volume_overlay paints a
 * top-center pill; the last VOL_OVERLAY_FADE_TAIL seconds linearly
 * fade alpha. */
#define VOL_OVERLAY_LIFETIME   2.0f
#define VOL_OVERLAY_FADE_TAIL  0.5f
static float g_vol_overlay_timer = 0.0f;

static float bus_resolved(AudioBusId bus) {
    if (g_muted) return 0.0f;
    if (bus < 0 || bus >= AUDIO_BUS_COUNT) bus = AUDIO_BUS_SFX;
    return g_buses[AUDIO_BUS_MASTER].gain *
           g_buses[bus].gain *
           g_buses[bus].duck_target;
}

/* audio_toggle_mute is defined further down, after the music /
 * ambient / servo file-statics it operates on. Forward declared
 * here so anything in this translation unit can call it (and so
 * the header order matches the public API order). */
bool audio_is_muted(void) {
    return g_muted;
}

void audio_draw_mute_overlay(int sw, int sh) {
    if (!g_muted) return;
    /* Small top-center pill: dark scrim + white "F2 MUTED" label.
     * Sized for the 1920×1080 default (scaled down at smaller
     * resolutions). raylib's Vector2 / Color types are already in
     * scope via the raylib.h include above. */
    (void)sh;
    float sc = (float)sw / 1920.0f;
    if (sc < 0.5f) sc = 0.5f;
    if (sc > 2.0f) sc = 2.0f;

    const char *label = "F2  MUTED";
    int font_px = (int)(18.0f * sc);
    if (font_px < 12) font_px = 12;

    int text_w = MeasureText(label, font_px);
    int pad_x  = (int)(12.0f * sc);
    int pad_y  = (int)(6.0f  * sc);
    int box_w  = text_w + 2 * pad_x;
    int box_h  = font_px + 2 * pad_y;
    int x      = (sw - box_w) / 2;
    int y      = (int)(8.0f * sc);

    DrawRectangle(x, y, box_w, box_h, (Color){ 12, 14, 18, 220 });
    DrawRectangleLines(x, y, box_w, box_h, (Color){ 220, 64, 64, 255 });
    DrawText(label, x + pad_x, y + pad_y, font_px,
             (Color){ 240, 240, 240, 255 });
}

void audio_set_bus_volume(AudioBusId bus, float v) {
    if (bus < 0 || bus >= AUDIO_BUS_COUNT) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_buses[bus].gain = v;
}

float audio_get_bus_volume(AudioBusId bus) {
    if (bus < 0 || bus >= AUDIO_BUS_COUNT) return 0.0f;
    return g_buses[bus].gain;
}

bool audio_master_volume_nudge(float delta) {
    /* F2-mute interaction: pressing '+' while muted is the universal
     * "give me sound back" gesture — un-mute but don't move the gain
     * (the user's previously chosen volume is restored). Pressing
     * '-' while muted is a no-op: we're already at zero output, so
     * dropping the underlying gain would be invisible and the
     * overlay would lie about why nothing is audible. */
    if (g_muted) {
        if (delta > 0.0f) {
            audio_toggle_mute();
            g_vol_overlay_timer = VOL_OVERLAY_LIFETIME;
            return true;
        }
        return false;
    }
    float prev = g_buses[AUDIO_BUS_MASTER].gain;
    float v    = prev + delta;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_buses[AUDIO_BUS_MASTER].gain = v;
    g_vol_overlay_timer = VOL_OVERLAY_LIFETIME;
    return v != prev;
}

void audio_draw_volume_overlay(int sw, int sh) {
    if (g_vol_overlay_timer <= 0.0f) return;
    (void)sh;

    float sc = (float)sw / 1920.0f;
    if (sc < 0.5f) sc = 0.5f;
    if (sc > 2.0f) sc = 2.0f;

    /* Fade out the last VOL_OVERLAY_FADE_TAIL seconds; hold full
     * opacity before that. Same shape as draw_kill_feed in src/hud.c. */
    float a_frac = (g_vol_overlay_timer < VOL_OVERLAY_FADE_TAIL)
                   ? (g_vol_overlay_timer / VOL_OVERLAY_FADE_TAIL)
                   : 1.0f;
    if (a_frac < 0.0f) a_frac = 0.0f;
    unsigned char a_scrim = (unsigned char)(a_frac * 220.0f);
    unsigned char a_fg    = (unsigned char)(a_frac * 240.0f);
    unsigned char a_acc   = (unsigned char)(a_frac * 200.0f);

    int   pct      = (int)(g_buses[AUDIO_BUS_MASTER].gain * 100.0f + 0.5f);
    char  label[32];
    snprintf(label, sizeof label, "VOL  %3d%%", pct);

    int font_px = (int)(18.0f * sc);
    if (font_px < 12) font_px = 12;

    int text_w = MeasureText(label, font_px);
    int pad_x  = (int)(12.0f * sc);
    int pad_y  = (int)(6.0f  * sc);
    int bar_w  = (int)(120.0f * sc);
    int bar_h  = (int)(8.0f   * sc);
    int gap    = (int)(10.0f  * sc);
    int box_w  = text_w + bar_w + 2 * pad_x + gap;
    int box_h  = font_px + 2 * pad_y;
    int x      = (sw - box_w) / 2;
    /* Park below the F2 MUTED pill if it's currently drawn so the two
     * overlays don't overlap. Mute pill is at y = 8*sc with the same
     * height; leave a small gap between them. */
    int y_top  = (int)(8.0f * sc);
    if (g_muted) y_top += box_h + (int)(4.0f * sc);

    DrawRectangle(x, y_top, box_w, box_h, (Color){ 12, 14, 18, a_scrim });
    DrawRectangleLines(x, y_top, box_w, box_h, (Color){ 80, 160, 220, a_acc });
    DrawText(label, x + pad_x, y_top + pad_y, font_px,
             (Color){ 240, 240, 240, a_fg });

    int bar_x = x + pad_x + text_w + gap;
    int bar_y = y_top + (box_h - bar_h) / 2;
    DrawRectangle(bar_x, bar_y, bar_w, bar_h, (Color){ 40, 44, 52, a_scrim });
    int fill = (int)(g_buses[AUDIO_BUS_MASTER].gain * (float)bar_w + 0.5f);
    if (fill > 0) {
        DrawRectangle(bar_x, bar_y, fill, bar_h,
                      (Color){ 80, 200, 240, a_fg });
    }
    DrawRectangleLines(bar_x, bar_y, bar_w, bar_h, (Color){ 80, 160, 220, a_acc });
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
    double     last_play_t;         /* m6-ui-fixes — GetTime() of the last
                                     * audio_play_global hit; drives the
                                     * UI-bus minimum-interval guard. */
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
    /* M6 P04 — damage-number bounce. 0.30 sits well below SFX_HIT_METAL
     * (0.85) so the tink reads as the secondary "and now the digit
     * bounces around" beat, not as another impact. */
    { SFX_DAMAGE_TINK,       "assets/sfx/damage_tink.wav",   3, 0.30f, AUDIO_BUS_SFX },

    /* --- Explosions --- */
    { SFX_EXPLOSION_LARGE,   "assets/sfx/explosion_large.wav",  3, 1.00f, AUDIO_BUS_SFX },
    { SFX_EXPLOSION_MEDIUM,  "assets/sfx/explosion_medium.wav", 3, 0.90f, AUDIO_BUS_SFX },
    { SFX_EXPLOSION_SMALL,   "assets/sfx/explosion_small.wav",  3, 0.80f, AUDIO_BUS_SFX },

    /* --- Movement --- */
    /* Footsteps at 0.70 (iterations: 0.55 → 0.25 → 0.40 → 0.70).
     * The servo loop is now silent (SERVO_MAX_VOL=0), so the
     * footstep is the entire walking cue. 0.70 puts it on par with
     * other gameplay-critical SFX (pulse_rifle is 0.85, hit_flesh
     * 0.90) — loud enough that enemy footsteps at mid-range
     * (~500–800 px) are clearly audible for competitive listening.
     * Sources are trimmed to ~120 ms in source_map.sh so per-foot
     * plants don't overlap. */
    { SFX_FOOTSTEP_METAL,    "assets/sfx/footstep_metal.wav",    5, 0.70f, AUDIO_BUS_SFX },
    { SFX_FOOTSTEP_CONCRETE, "assets/sfx/footstep_concrete.wav", 5, 0.70f, AUDIO_BUS_SFX },
    { SFX_FOOTSTEP_ICE,      "assets/sfx/footstep_ice.wav",      5, 0.70f, AUDIO_BUS_SFX },
    { SFX_JET_PULSE,         "assets/sfx/jet_pulse.wav",         5, 0.55f, AUDIO_BUS_SFX },
    { SFX_JET_BOOST,         "assets/sfx/jet_boost.wav",         3, 0.80f, AUDIO_BUS_SFX },
    /* M6 P02 — ignition cues. 3 aliases each, 0.85 SFX volume to sit
     * above jet_pulse (0.55) without overpowering explosion cues. */
    { SFX_JET_IGNITION_CONCRETE, "assets/sfx/jet_ignition_concrete.wav", 3, 0.85f, AUDIO_BUS_SFX },
    { SFX_JET_IGNITION_ICE,      "assets/sfx/jet_ignition_ice.wav",      3, 0.85f, AUDIO_BUS_SFX },
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

    /* --- UI ---
     * m6-ui-fixes — UI samples drop to 1 alias each. The previous
     * 3-alias round-robin was meant to let rapid plays overlap
     * (necessary for gunfire), but for UI clicks it's the source
     * of the audible "stacking": tapping two buttons in close
     * succession produced two simultaneous decays of the 103 ms
     * sample. One alias means PlaySound restarts from the start
     * each time, which is the standard click-UX shape and matches
     * what every other in-game UI does. The new debounce guard in
     * audio_play_global also filters same-frame double-fires that
     * happen when both a hot-rect handler AND a follow-up state
     * change ping the click within ~16 ms. */
    { SFX_UI_HOVER,          "assets/sfx/ui_hover.wav",          1, 0.45f, AUDIO_BUS_UI  },
    { SFX_UI_CLICK,          "assets/sfx/ui_click.wav",          1, 0.65f, AUDIO_BUS_UI  },
    { SFX_UI_TOGGLE,         "assets/sfx/ui_toggle.wav",         1, 0.55f, AUDIO_BUS_UI  },

    /* --- Death --- */
    { SFX_KILL_FANFARE,      "assets/sfx/kill_fanfare.wav",      3, 0.75f, AUDIO_BUS_SFX },
    { SFX_DEATH_GRUNT,       "assets/sfx/death_grunt.wav",       3, 0.80f, AUDIO_BUS_SFX },

    /* --- M6 countdown-fix — pre-round race-start cues --- */
    { SFX_COUNTDOWN_BEEP,    "assets/sfx/countdown_beep.wav",    2, 0.65f, AUDIO_BUS_UI  },
    { SFX_COUNTDOWN_GO,      "assets/sfx/countdown_go.wav",      2, 0.85f, AUDIO_BUS_UI  },
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
    /* Skip UpdateMusicStream while muted — the stream is paused by
     * audio_toggle_mute so there's nothing to advance. SetMusicVolume
     * is still safe to call on a paused stream, but is also
     * unnecessary. */
    if (g_muted) return;
    UpdateMusicStream(g_music);
    SetMusicVolume(g_music, bus_resolved(AUDIO_BUS_MUSIC));
}

static void ambient_step(void) {
    if (!g_ambient_loaded) return;
    /* Don't retrigger the ambient loop while muted. audio_toggle_mute
     * calls StopSound on mute; this gate keeps us from re-firing it
     * at volume 0 every tick (wasteful but otherwise harmless). */
    if (g_muted) return;
    if (!IsSoundPlaying(g_ambient)) PlaySound(g_ambient);
    SetSoundVolume(g_ambient, bus_resolved(AUDIO_BUS_AMBIENT));
}

void audio_step(struct World *w, float dt) {
    duck_step(dt);
    music_step();
    ambient_step();

    if (g_vol_overlay_timer > 0.0f) {
        g_vol_overlay_timer -= dt;
        if (g_vol_overlay_timer < 0.0f) g_vol_overlay_timer = 0.0f;
    }

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

    /* m6-ui-fixes — same-id minimum interval. Prevents the audible
     * "stacking" when two UI hot-rect handlers in the same frame both
     * fire SFX_UI_CLICK (e.g. closing one modal + opening another) or
     * when held repeat keys nudge audio_play_global faster than the
     * sample can decay. UI bus uses 60 ms; non-UI buses fall through
     * with no guard so concurrent gunfire / explosions still layer. */
    if (e->bus == AUDIO_BUS_UI) {
        double now = GetTime();
        if (now - e->last_play_t < 0.060) return;
        e->last_play_t = now;
    }

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
    /* Stop tracking velocity → volume while muted; audio_toggle_mute
     * already stopped the underlying Sound and zeroed current_vol.
     * The next unmute frame picks back up from a clean state. */
    if (g_muted) return;
    float t = clampf(velocity_pxs / SERVO_RUN_SPEED, 0.0f, 1.0f);
    g_servo_target_vol = t * SERVO_MAX_VOL;
    g_servo_current_vol += (g_servo_target_vol - g_servo_current_vol) * SERVO_LERP;

    /* Hard cutoff when motion has stopped — the user reported the
     * lerp tail was still audible after a round ended. Bumped the
     * threshold from 0.005 → 0.04 (eight× higher) so the snap-to-
     * zero fires earlier in the decay curve when target_vol is 0.
     * Above the cutoff the lerp still gives a smooth ramp-down feel;
     * below it we shut up immediately and the underlying Sound (at
     * volume 0) finishes its current play without retrigger. */
    if (g_servo_target_vol == 0.0f && g_servo_current_vol < 0.04f) {
        g_servo_current_vol = 0.0f;
        StopSound(g_servo);
    } else if (g_servo_current_vol < 0.005f) {
        g_servo_current_vol = 0.0f;
    }

    SetSoundVolume(g_servo, g_servo_current_vol * bus_resolved(AUDIO_BUS_SFX));
    if (!IsSoundPlaying(g_servo) && g_servo_current_vol > 0.0f) PlaySound(g_servo);
}

/* ---- Music --------------------------------------------------------- */

/* Resolve a STRT-indexed asset path into an absolute / assets/-prefixed
 * path. Used by audio_apply_for_level for music + ambient. Mirrors
 * main.c's previously-static resolve_meta_path; moved here so both
 * the host and the client round-start paths can apply per-map audio
 * without duplicating the logic. */
static const char *resolve_level_string_path(const Level *L, uint16_t idx,
                                             char *buf, size_t cap)
{
    if (idx == 0) return NULL;
    if (!L->string_table) return NULL;
    if ((int)idx >= L->string_table_size) return NULL;
    const char *s = L->string_table + idx;
    if (!*s) return NULL;
    if (s[0] == '/' || strncmp(s, "assets/", 7) == 0) return s;
    snprintf(buf, cap, "assets/%s", s);
    return buf;
}

void audio_apply_for_level(const Level *L) {
    if (!L) return;
    char music_buf[256];
    char ambient_buf[256];
    const char *music_path = resolve_level_string_path(L, L->meta.music_str_idx,
                                                       music_buf, sizeof music_buf);
    audio_set_music_for_map(music_path);
    if (music_path) audio_music_play();

    const char *ambient_path = resolve_level_string_path(L,
                                                          L->meta.ambient_loop_str_idx,
                                                          ambient_buf,
                                                          sizeof ambient_buf);
    audio_set_ambient_loop(ambient_path);
}

void audio_apply_for_title(void) {
    /* Mirror of audio_apply_for_level but with NULL paths — stops and
     * unloads both the music stream and the ambient loop sample.
     * audio_set_*_for_map(NULL) is idempotent (no-op when nothing's
     * loaded) so this is safe to call from any leave path even if
     * the bot user never entered a match in this session. */
    audio_set_music_for_map(NULL);
    audio_set_ambient_loop(NULL);
}

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

/* ---- F2 mute toggle ----------------------------------------------- */
/* Defined here (vs. up by `bus_resolved`) because we need to touch
 * the music / ambient / servo file-statics declared in the sections
 * above. `audio_is_muted` lives next to `bus_resolved` for callers
 * that just want to read the flag. */

void audio_toggle_mute(void) {
    g_muted = !g_muted;
    /* Pause/Resume the music stream explicitly. SetMusicVolume(0) is
     * not reliable across raylib versions — some leave the stream
     * playing-but-silent (correct), some halt decoding entirely
     * (which is what bit us: unmuting restored volume but the
     * stream was no longer advancing, so music stayed silent until
     * the next map-switch reloaded the stream). Pause is the only
     * unambiguous signal.
     *
     * For ambient + servo (raylib `Sound` instances driven by
     * per-frame retrigger), call StopSound on mute so the
     * retrigger logic in ambient_step + audio_servo_update doesn't
     * re-fire them at volume 0 every tick. Unmute lets the next
     * frame retrigger them fresh. */
    if (g_music_loaded) {
        if (g_muted) PauseMusicStream(g_music);
        else         ResumeMusicStream(g_music);
    }
    if (g_muted) {
        if (g_ambient_loaded) StopSound(g_ambient);
        if (g_servo_loaded)   { StopSound(g_servo); g_servo_current_vol = 0.0f; }
    }
    LOG_I("audio: %s", g_muted ? "muted (F2)" : "unmuted (F2)");
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

/* ---- Tooling enumeration ------------------------------------------ */

int audio_manifest_count(void) {
    return SFX_MANIFEST_COUNT;
}

const char *audio_manifest_path(int idx) {
    if (idx < 0 || idx >= SFX_MANIFEST_COUNT) return NULL;
    return g_sfx_manifest[idx].path;
}

const char *audio_manifest_kind(int idx) {
    if (idx < 0 || idx >= SFX_MANIFEST_COUNT) return NULL;
    SfxId id = g_sfx_manifest[idx].id;
    if (id <= SFX_WPN_KNIFE_THROW)    return "SFX_WPN";
    if (id <= SFX_DAMAGE_TINK)        return "SFX_HIT";
    if (id <= SFX_EXPLOSION_SMALL)    return "SFX_EXPLOSION";
    if (id <= SFX_FOOTSTEP_ICE)       return "SFX_FOOT";
    if (id <= SFX_JET_IGNITION_ICE)   return "SFX_JET";
    if (id <= SFX_LANDING_SOFT)       return "SFX_LANDING";
    if (id <= SFX_PICKUP_RESPAWN)     return "SFX_PICKUP";
    if (id <= SFX_GRAPPLE_PULL_LOOP)  return "SFX_GRAPPLE";
    if (id <= SFX_FLAG_CAPTURE)       return "SFX_FLAG";
    if (id <= SFX_UI_TOGGLE)          return "SFX_UI";
    return "SFX_KILL";
}

const char *audio_servo_path(void) {
    return SERVO_PATH;
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
