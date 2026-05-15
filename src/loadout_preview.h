#pragma once

#include "world.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdbool.h>

/*
 * loadout_preview — M6 lobby loadout preview modal.
 *
 * Renders the current loadout (chassis + primary + secondary + armor +
 * jetpack) on a "treadmill" cycling STAND → RUN → JET, alongside a
 * radar/spider chart of six axes (Speed / Armor / Firepower / Range /
 * Mobility / Utility) and a composed description blurb. Cycle buttons
 * inside the modal let the player shop between options without closing.
 *
 * Lifecycle: init at startup (zeros struct + binds the internal particle
 * pool); shutdown at exit (unloads the offscreen render target). The
 * render target is created lazily on first open so headless / shotmode
 * runs that never click Preview don't allocate one.
 *
 * Inputs: per-frame sw / sh / dt from the host UI loop. Loadout id
 * pointers are mutated directly by the in-modal cycle buttons; the
 * caller writes the change back to lobby/server state.
 */

struct UIContext;

typedef struct LoadoutPreview {
    bool             open;
    bool             rt_ready;

    /* Synthetic mech driven by pose_compute() each frame. Particle data
     * lives inline so the preview can run without touching the live
     * World's particle pool. */
    Mech             mech;
    ParticlePool     particles;
    ConstraintPool   constraints;
    float            buf_pos_x      [PART_COUNT];
    float            buf_pos_y      [PART_COUNT];
    float            buf_prev_x     [PART_COUNT];
    float            buf_prev_y     [PART_COUNT];
    float            buf_render_prev_x[PART_COUNT];
    float            buf_render_prev_y[PART_COUNT];
    float            buf_inv_mass   [PART_COUNT];
    uint8_t          buf_flags      [PART_COUNT];
    int8_t           buf_contact_nx_q[PART_COUNT];
    int8_t           buf_contact_ny_q[PART_COUNT];
    uint8_t          buf_contact_kind[PART_COUNT];

    /* Animation cycle timer (seconds since modal opened). */
    float            cycle_time;
    float            gait_phase;

    /* Offscreen render target for the treadmill panel. Created lazily
     * on first open so the cost is paid only when the player asks. */
    RenderTexture2D  rt;
    int              rt_w, rt_h;

    /* Pulse timer for the lobby's Preview button glow. Independent of
     * cycle_time so the button keeps pulsing even when the modal is
     * closed. */
    double           pulse_base_time;
} LoadoutPreview;

void loadout_preview_init    (LoadoutPreview *st);
void loadout_preview_shutdown(LoadoutPreview *st);

bool loadout_preview_is_open(const LoadoutPreview *st);
void loadout_preview_open   (LoadoutPreview *st);
void loadout_preview_close  (LoadoutPreview *st);

/* Eye-catching button drawn under the jetpack picker. Returns true on
 * the press-edge that lands inside. Pulses with a distinct cyan accent
 * so the player's eye picks it up as the discoverable "see what you've
 * got" affordance. `enabled = false` dims the chrome and ignores
 * clicks (used while the player is in the READY state — same gating
 * as the loadout cycle buttons). */
bool loadout_preview_draw_button(LoadoutPreview *st, struct UIContext *u,
                                 Rectangle rect, bool enabled);

/* Update + render the modal — no-op when closed. Returns true on any
 * tick where the loadout was changed by an in-modal cycle button so
 * the caller can re-push the loadout to the server. The loadout ids
 * are mutated in place. `can_edit` mirrors the lobby's `!me_ready` —
 * when false the in-modal cycle buttons render disabled (same chrome
 * as the lobby's disabled cycles) so a player who Ready-Up'd can still
 * SEE their loadout but not change it. */
bool loadout_preview_update_and_draw(LoadoutPreview *st,
                                     struct UIContext *u,
                                     int sw, int sh, float dt,
                                     bool can_edit,
                                     int *chassis_id,
                                     int *primary_id,
                                     int *secondary_id,
                                     int *armor_id,
                                     int *jetpack_id);
