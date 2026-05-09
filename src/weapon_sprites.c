#include "weapon_sprites.h"

#include "log.h"

/* The atlas is a single shared texture for all 14 weapons. Indexed by
 * WeaponId; each entry's `src` lives inside the atlas's coordinate
 * space. Until `assets/sprites/weapons.png` ships (P16), the renderer
 * sees `id == 0` and falls back to per-weapon line draws sized by
 * `draw_w * 0.7`. */
Texture2D g_weapons_atlas = {0};

/* Pixel-level placeholder atlas layout. Three rows packed into a
 * 1024×128 region (whole atlas is sized 1024×256 to leave headroom for
 * P16 muzzle-flash sprites). Sizes per documents/m5/12-rigging-and-
 * damage.md §"Per-weapon visible sizes"; pivots per
 * §"Foregrip positions per two-handed weapon".
 *
 *  Row 1 (y=0):    Pulse Rifle 56×14 | Plasma SMG 48×16 | Riot 60×24 |
 *                  Rail 88×16 | Auto-Cannon 60×16
 *  Row 2 (y=40):   Mass Driver 96×32 | Plasma Cannon 64×22 |
 *                  Microgun 80×28 | Sidearm 28×10 | Burst SMG 44×14
 *  Row 3 (y=80):   Frag 16×16 | Micro-Rockets 52×20 | Knife 32×8 |
 *                  Grappling Hook 36×14
 *
 * One-handed weapons (Sidearm / Frag / Knife / Grapple) carry
 * `pivot_foregrip = (-1, -1)`. The renderer reads pivot_grip as the
 * `DrawTexturePro` origin so the grip pixel aligns with R_HAND. */
const WeaponSpriteDef g_weapon_sprites[WEAPON_COUNT] = {
    /* --- Primaries --- */
    [WEAPON_PULSE_RIFLE] = {
        .src            = {   0,   0,  56,  14 },
        .pivot_grip     = {   8,   7 },
        .pivot_foregrip = {  32,   7 },
        .muzzle_offset  = {  56,   7 },
        .draw_w = 56.0f, .draw_h = 14.0f,
        .weapon_id = WEAPON_PULSE_RIFLE,
    },
    [WEAPON_PLASMA_SMG] = {
        .src            = {  60,   0,  48,  16 },
        .pivot_grip     = {   6,   8 },
        .pivot_foregrip = {  24,   8 },
        .muzzle_offset  = {  48,   8 },
        .draw_w = 48.0f, .draw_h = 16.0f,
        .weapon_id = WEAPON_PLASMA_SMG,
    },
    [WEAPON_RIOT_CANNON] = {
        .src            = { 110,   0,  60,  24 },
        .pivot_grip     = {   8,  14 },
        .pivot_foregrip = {  28,  14 },
        .muzzle_offset  = {  60,  14 },
        .draw_w = 60.0f, .draw_h = 24.0f,
        .weapon_id = WEAPON_RIOT_CANNON,
    },
    [WEAPON_RAIL_CANNON] = {
        .src            = { 175,   0,  88,  16 },
        .pivot_grip     = {   8,   8 },
        .pivot_foregrip = {  32,   8 },
        .muzzle_offset  = {  88,   8 },
        .draw_w = 88.0f, .draw_h = 16.0f,
        .weapon_id = WEAPON_RAIL_CANNON,
    },
    [WEAPON_AUTO_CANNON] = {
        .src            = { 270,   0,  60,  16 },
        .pivot_grip     = {   8,   8 },
        .pivot_foregrip = {  28,   8 },
        .muzzle_offset  = {  60,   8 },
        .draw_w = 60.0f, .draw_h = 16.0f,
        .weapon_id = WEAPON_AUTO_CANNON,
    },
    [WEAPON_MASS_DRIVER] = {
        .src            = {   0,  40,  96,  32 },
        .pivot_grip     = {  12,  18 },
        .pivot_foregrip = {  40,  18 },
        .muzzle_offset  = {  96,  16 },
        .draw_w = 96.0f, .draw_h = 32.0f,
        .weapon_id = WEAPON_MASS_DRIVER,
    },
    [WEAPON_PLASMA_CANNON] = {
        .src            = { 100,  40,  64,  22 },
        .pivot_grip     = {   8,  12 },
        .pivot_foregrip = {  28,  12 },
        .muzzle_offset  = {  64,  12 },
        .draw_w = 64.0f, .draw_h = 22.0f,
        .weapon_id = WEAPON_PLASMA_CANNON,
    },
    [WEAPON_MICROGUN] = {
        .src            = { 170,  40,  80,  28 },
        .pivot_grip     = {  10,  14 },
        .pivot_foregrip = {  32,  14 },
        .muzzle_offset  = {  80,  14 },
        .draw_w = 80.0f, .draw_h = 28.0f,
        .weapon_id = WEAPON_MICROGUN,
    },

    /* --- Secondaries --- */
    [WEAPON_SIDEARM] = {
        .src            = { 260,  40,  28,  10 },
        .pivot_grip     = {   6,   5 },
        .pivot_foregrip = {  -1,  -1 },        /* one-handed */
        .muzzle_offset  = {  28,   5 },
        .draw_w = 28.0f, .draw_h = 10.0f,
        .weapon_id = WEAPON_SIDEARM,
    },
    [WEAPON_BURST_SMG] = {
        .src            = { 300,  40,  44,  14 },
        .pivot_grip     = {   6,   7 },
        .pivot_foregrip = {  22,   7 },
        .muzzle_offset  = {  44,   7 },
        .draw_w = 44.0f, .draw_h = 14.0f,
        .weapon_id = WEAPON_BURST_SMG,
    },
    [WEAPON_FRAG_GRENADES] = {
        .src            = {   0,  80,  16,  16 },
        .pivot_grip     = {   8,   8 },
        .pivot_foregrip = {  -1,  -1 },        /* one-handed throw */
        .muzzle_offset  = {   8,   8 },        /* throw exits at the hand */
        .draw_w = 16.0f, .draw_h = 16.0f,
        .weapon_id = WEAPON_FRAG_GRENADES,
    },
    [WEAPON_MICRO_ROCKETS] = {
        .src            = {  24,  80,  52,  20 },
        .pivot_grip     = {   8,  10 },
        .pivot_foregrip = {  26,  10 },
        .muzzle_offset  = {  52,  10 },
        .draw_w = 52.0f, .draw_h = 20.0f,
        .weapon_id = WEAPON_MICRO_ROCKETS,
    },
    [WEAPON_COMBAT_KNIFE] = {
        .src            = {  84,  80,  32,   8 },
        .pivot_grip     = {   6,   4 },
        .pivot_foregrip = {  -1,  -1 },        /* one-handed */
        .muzzle_offset  = {  32,   4 },        /* tip of the blade */
        .draw_w = 32.0f, .draw_h =  8.0f,
        .weapon_id = WEAPON_COMBAT_KNIFE,
    },
    [WEAPON_GRAPPLING_HOOK] = {
        .src            = { 124,  80,  36,  14 },
        .pivot_grip     = {   6,   7 },
        .pivot_foregrip = {  -1,  -1 },        /* one-handed wrist mount */
        .muzzle_offset  = {  36,   7 },
        .draw_w = 36.0f, .draw_h = 14.0f,
        .weapon_id = WEAPON_GRAPPLING_HOOK,
    },
};

const WeaponSpriteDef *weapon_sprite_def(int weapon_id) {
    if ((unsigned)weapon_id >= WEAPON_COUNT) return NULL;
    return &g_weapon_sprites[weapon_id];
}

Vec2 weapon_muzzle_world(Vec2 hand, Vec2 aim, const WeaponSpriteDef *wp,
                         float fallback_offset)
{
    if (!wp) {
        return (Vec2){ hand.x + aim.x * fallback_offset,
                       hand.y + aim.y * fallback_offset };
    }
    /* Sprite-local axes: +X along the barrel (grip → muzzle), +Y
     * perpendicular (down on the source image). World-space transform
     * uses `aim` as +X and the right-hand perpendicular (-aim.y, aim.x)
     * as +Y. Same convention as the foregrip helper below so both
     * coincide on the same sprite line. */
    float along = wp->muzzle_offset.x - wp->pivot_grip.x;
    float perp  = wp->muzzle_offset.y - wp->pivot_grip.y;
    return (Vec2){
        hand.x + aim.x * along + (-aim.y) * perp,
        hand.y + aim.y * along + ( aim.x) * perp,
    };
}

bool weapon_foregrip_world(Vec2 hand, Vec2 aim, const WeaponSpriteDef *wp,
                           Vec2 *out)
{
    if (!wp || !out) return false;
    if (wp->pivot_foregrip.x < 0.0f) return false;     /* one-handed */
    float along = wp->pivot_foregrip.x - wp->pivot_grip.x;
    float perp  = wp->pivot_foregrip.y - wp->pivot_grip.y;
    *out = (Vec2){
        hand.x + aim.x * along + (-aim.y) * perp,
        hand.y + aim.y * along + ( aim.x) * perp,
    };
    return true;
}

bool weapons_atlas_load(void) {
    if (g_weapons_atlas.id != 0) return true;          /* idempotent */
    const char *path = "assets/sprites/weapons.png";
    if (!FileExists(path)) {
        LOG_I("weapons_atlas: %s not found — fallback line render",
              path);
        return false;
    }
    Texture2D tex = LoadTexture(path);
    if (tex.id == 0) {
        LOG_W("weapons_atlas: failed to load %s — fallback line render",
              path);
        return false;
    }
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    g_weapons_atlas = tex;
    LOG_I("weapons_atlas: loaded %s (%dx%d)",
          path, tex.width, tex.height);
    return true;
}

void weapons_atlas_unload(void) {
    if (g_weapons_atlas.id != 0) {
        UnloadTexture(g_weapons_atlas);
        g_weapons_atlas = (Texture2D){0};
    }
}
