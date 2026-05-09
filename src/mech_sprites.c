#include "mech_sprites.h"

#include "log.h"

#include <stdio.h>
#include <string.h>

/* The renderer reads this directly. `atlas.id == 0` means "no atlas
 * loaded for this chassis" and the renderer falls back to the M4
 * capsule path. `parts[]` is populated to a sane Trooper-sized
 * placeholder by `mech_sprites_load_all` so the sprite path produces
 * something coherent if a single chassis PNG is dropped during
 * integration. P15 / P16 will replace these placeholders with
 * per-chassis hand-packed sub-rects/pivots transcribed from rTexPacker. */
MechSpriteSet g_chassis_sprites[CHASSIS_COUNT];

/* Default parts table — pixel coords for a notional Trooper-sized
 * 1024×1024 atlas. All five chassis copy this layout at load time;
 * the differentiation between chassis comes from the per-chassis bone
 * lengths in `g_chassis[]` (which drive constraint rest lengths and
 * pose targets) until per-chassis art lands.
 *
 * Sub-rect sizes track `documents/m5/08-rendering.md` §"Atlas layout":
 *   torso 56×72, head 40×40, hip 64×36, shoulder L/R 56×40,
 *   arm_upper L/R 32×80, arm_lower L/R 28×72, hand L/R 16×16,
 *   leg_upper L/R 36×96, leg_lower L/R 32×88, foot L/R 32×24,
 *   stump caps × 5 each 32×32.
 *
 * Pivots are centered: limb sprites pivot at the bone-segment midpoint,
 * plates pivot at their center over the parent particle, stumps pivot
 * at center over the parent. P12 may shift the plate pivots if the
 * authored art needs them off-center. */
static const MechSpritePart s_default_parts[MSP_COUNT] = {
    [MSP_TORSO]            = { .src = {  0,   0,  56,  72 }, .pivot = { 28, 36 }, .draw_w =  56, .draw_h =  72 },
    [MSP_HEAD]             = { .src = { 60,   0,  40,  40 }, .pivot = { 20, 20 }, .draw_w =  40, .draw_h =  40 },
    [MSP_HIP_PLATE]        = { .src = {104,   0,  64,  36 }, .pivot = { 32, 18 }, .draw_w =  64, .draw_h =  36 },
    [MSP_SHOULDER_L]       = { .src = {172,   0,  56,  40 }, .pivot = { 28, 20 }, .draw_w =  56, .draw_h =  40 },
    [MSP_SHOULDER_R]       = { .src = {232,   0,  56,  40 }, .pivot = { 28, 20 }, .draw_w =  56, .draw_h =  40 },

    [MSP_LEG_UPPER_L]      = { .src = {  0,  80,  36,  96 }, .pivot = { 18, 48 }, .draw_w =  36, .draw_h =  96 },
    [MSP_LEG_UPPER_R]      = { .src = { 40,  80,  36,  96 }, .pivot = { 18, 48 }, .draw_w =  36, .draw_h =  96 },
    [MSP_LEG_LOWER_L]      = { .src = { 80,  80,  32,  88 }, .pivot = { 16, 44 }, .draw_w =  32, .draw_h =  88 },
    [MSP_LEG_LOWER_R]      = { .src = {116,  80,  32,  88 }, .pivot = { 16, 44 }, .draw_w =  32, .draw_h =  88 },
    [MSP_FOOT_L]           = { .src = {152,  80,  32,  24 }, .pivot = { 16, 12 }, .draw_w =  32, .draw_h =  24 },
    [MSP_FOOT_R]           = { .src = {188,  80,  32,  24 }, .pivot = { 16, 12 }, .draw_w =  32, .draw_h =  24 },

    [MSP_ARM_UPPER_L]      = { .src = {  0, 180,  32,  80 }, .pivot = { 16, 40 }, .draw_w =  32, .draw_h =  80 },
    [MSP_ARM_UPPER_R]      = { .src = { 36, 180,  32,  80 }, .pivot = { 16, 40 }, .draw_w =  32, .draw_h =  80 },
    [MSP_ARM_LOWER_L]      = { .src = { 72, 180,  28,  72 }, .pivot = { 14, 36 }, .draw_w =  28, .draw_h =  72 },
    [MSP_ARM_LOWER_R]      = { .src = {104, 180,  28,  72 }, .pivot = { 14, 36 }, .draw_w =  28, .draw_h =  72 },
    [MSP_HAND_L]           = { .src = {136, 180,  16,  16 }, .pivot = {  8,  8 }, .draw_w =  16, .draw_h =  16 },
    [MSP_HAND_R]           = { .src = {156, 180,  16,  16 }, .pivot = {  8,  8 }, .draw_w =  16, .draw_h =  16 },

    [MSP_STUMP_SHOULDER_L] = { .src = {  0, 264,  32,  32 }, .pivot = { 16, 16 }, .draw_w =  32, .draw_h =  32 },
    [MSP_STUMP_SHOULDER_R] = { .src = { 36, 264,  32,  32 }, .pivot = { 16, 16 }, .draw_w =  32, .draw_h =  32 },
    [MSP_STUMP_HIP_L]      = { .src = { 72, 264,  32,  32 }, .pivot = { 16, 16 }, .draw_w =  32, .draw_h =  32 },
    [MSP_STUMP_HIP_R]      = { .src = {108, 264,  32,  32 }, .pivot = { 16, 16 }, .draw_w =  32, .draw_h =  32 },
    [MSP_STUMP_NECK]       = { .src = {144, 264,  32,  32 }, .pivot = { 16, 16 }, .draw_w =  32, .draw_h =  32 },
};

static const char *s_chassis_basenames[CHASSIS_COUNT] = {
    [CHASSIS_TROOPER]  = "trooper",
    [CHASSIS_SCOUT]    = "scout",
    [CHASSIS_HEAVY]    = "heavy",
    [CHASSIS_SNIPER]   = "sniper",
    [CHASSIS_ENGINEER] = "engineer",
};

bool mech_sprites_load_all(void) {
    bool any_loaded = false;
    for (int c = 0; c < CHASSIS_COUNT; ++c) {
        /* Always populate parts so a runtime that toggles the atlas in
         * (e.g. via a hot-reload path) doesn't read garbage rects. */
        memcpy(g_chassis_sprites[c].parts, s_default_parts,
               sizeof s_default_parts);
        if (g_chassis_sprites[c].atlas.id != 0) {
            any_loaded = true;
            continue;          /* idempotent on re-call */
        }
        char path[256];
        snprintf(path, sizeof path, "assets/sprites/%s.png",
                 s_chassis_basenames[c]);
        if (!FileExists(path)) {
            LOG_I("mech_sprites: %s not found — chassis %s renders as capsules",
                  path, s_chassis_basenames[c]);
            g_chassis_sprites[c].atlas.id = 0;
            continue;
        }
        Texture2D tex = LoadTexture(path);
        if (tex.id == 0) {
            LOG_W("mech_sprites: failed to load %s — chassis %s renders as capsules",
                  path, s_chassis_basenames[c]);
            continue;
        }
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
        g_chassis_sprites[c].atlas = tex;
        any_loaded = true;
        LOG_I("mech_sprites: loaded %s (%dx%d) for chassis %s",
              path, tex.width, tex.height, s_chassis_basenames[c]);
    }
    return any_loaded;
}

void mech_sprites_unload_all(void) {
    for (int c = 0; c < CHASSIS_COUNT; ++c) {
        if (g_chassis_sprites[c].atlas.id != 0) {
            UnloadTexture(g_chassis_sprites[c].atlas);
            g_chassis_sprites[c].atlas = (Texture2D){0};
        }
    }
}

/* P12 — Damage-decal helpers. The mapping below is the inverse of
 * `g_render_parts` in render.c: hits at a bone's distal particle
 * accumulate on the bone's sprite. Hits at the joints (shoulder, hip,
 * neck) accumulate on the plate sprite that covers that joint, so a
 * shoulder hit reads on the shoulder plate, not the upper-arm. Hits at
 * hand/foot particles accumulate on the lower-arm/leg sprite (the
 * hand/foot sprites are too small for visible decals). */
MechSpriteId mech_part_to_sprite_id(int part) {
    switch (part) {
        case PART_HEAD:       return MSP_HEAD;
        case PART_NECK:       return MSP_HEAD;
        case PART_CHEST:      return MSP_TORSO;
        case PART_PELVIS:     return MSP_HIP_PLATE;
        case PART_L_SHOULDER: return MSP_SHOULDER_L;
        case PART_R_SHOULDER: return MSP_SHOULDER_R;
        case PART_L_ELBOW:    return MSP_ARM_UPPER_L;
        case PART_R_ELBOW:    return MSP_ARM_UPPER_R;
        case PART_L_HAND:     return MSP_ARM_LOWER_L;
        case PART_R_HAND:     return MSP_ARM_LOWER_R;
        case PART_L_HIP:      return MSP_HIP_PLATE;
        case PART_R_HIP:      return MSP_HIP_PLATE;
        case PART_L_KNEE:     return MSP_LEG_UPPER_L;
        case PART_R_KNEE:     return MSP_LEG_UPPER_R;
        case PART_L_FOOT:     return MSP_LEG_LOWER_L;
        case PART_R_FOOT:     return MSP_LEG_LOWER_R;
        default:              return MSP_TORSO;
    }
}

void mech_sprite_part_endpoints(MechSpriteId sp, int *out_a, int *out_b) {
    int a = -1, b = PART_PELVIS;
    switch (sp) {
        case MSP_TORSO:        a = PART_CHEST;      b = PART_PELVIS;     break;
        case MSP_HEAD:         a = PART_NECK;       b = PART_HEAD;       break;
        case MSP_HIP_PLATE:    a = -1;              b = PART_PELVIS;     break;
        case MSP_SHOULDER_L:   a = -1;              b = PART_L_SHOULDER; break;
        case MSP_SHOULDER_R:   a = -1;              b = PART_R_SHOULDER; break;
        case MSP_ARM_UPPER_L:  a = PART_L_SHOULDER; b = PART_L_ELBOW;    break;
        case MSP_ARM_UPPER_R:  a = PART_R_SHOULDER; b = PART_R_ELBOW;    break;
        case MSP_ARM_LOWER_L:  a = PART_L_ELBOW;    b = PART_L_HAND;     break;
        case MSP_ARM_LOWER_R:  a = PART_R_ELBOW;    b = PART_R_HAND;     break;
        case MSP_HAND_L:       a = -1;              b = PART_L_HAND;     break;
        case MSP_HAND_R:       a = -1;              b = PART_R_HAND;     break;
        case MSP_LEG_UPPER_L:  a = PART_L_HIP;      b = PART_L_KNEE;     break;
        case MSP_LEG_UPPER_R:  a = PART_R_HIP;      b = PART_R_KNEE;     break;
        case MSP_LEG_LOWER_L:  a = PART_L_KNEE;     b = PART_L_FOOT;     break;
        case MSP_LEG_LOWER_R:  a = PART_R_KNEE;     b = PART_R_FOOT;     break;
        case MSP_FOOT_L:       a = -1;              b = PART_L_FOOT;     break;
        case MSP_FOOT_R:       a = -1;              b = PART_R_FOOT;     break;
        default:               a = -1;              b = PART_PELVIS;     break;
    }
    if (out_a) *out_a = a;
    if (out_b) *out_b = b;
}
