#include "map_kit.h"

#include "log.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <string.h>

MapKit g_map_kit = {0};

/* Try to load a single PNG into `slot`. Returns true on success; logs
 * INFO when missing (expected through P14 — assets ship at P15/P16) and
 * WARN when present-but-broken. */
static bool kit_load_one(Texture2D *slot, const char *path) {
    *slot = (Texture2D){0};
    if (!FileExists(path)) {
        return false;
    }
    Texture2D t = LoadTexture(path);
    if (t.id == 0) {
        LOG_W("map_kit: LoadTexture(%s) failed", path);
        return false;
    }
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    *slot = t;
    return true;
}

void map_kit_unload(void) {
    if (g_map_kit.parallax_far .id != 0) UnloadTexture(g_map_kit.parallax_far);
    if (g_map_kit.parallax_mid .id != 0) UnloadTexture(g_map_kit.parallax_mid);
    if (g_map_kit.parallax_near.id != 0) UnloadTexture(g_map_kit.parallax_near);
    if (g_map_kit.tiles        .id != 0) UnloadTexture(g_map_kit.tiles);
    g_map_kit = (MapKit){0};
}

void map_kit_load(const char *short_name) {
    if (!short_name || !*short_name) {
        map_kit_unload();
        return;
    }
    /* wan-fixes-5 — `--dedicated` runs without a raylib window / GL
     * context. raylib's LoadTexture crashes (or no-ops with garbage
     * id) without one, so skip kit-load when no window is ready. The
     * dedicated server never renders these textures itself; clients
     * load their own copies. */
    if (!IsWindowReady()) return;
    if (strncmp(g_map_kit.short_name, short_name,
                sizeof g_map_kit.short_name) == 0 &&
        (g_map_kit.parallax_far .id != 0 ||
         g_map_kit.parallax_mid .id != 0 ||
         g_map_kit.parallax_near.id != 0 ||
         g_map_kit.tiles        .id != 0))
    {
        /* Already loaded for this map. Idempotent. */
        return;
    }
    map_kit_unload();
    snprintf(g_map_kit.short_name, sizeof g_map_kit.short_name, "%s", short_name);

    char path[256];
    int loaded = 0;
    snprintf(path, sizeof path, "assets/maps/%s/parallax_far.png",  short_name);
    if (kit_load_one(&g_map_kit.parallax_far,  path)) loaded++;
    snprintf(path, sizeof path, "assets/maps/%s/parallax_mid.png",  short_name);
    if (kit_load_one(&g_map_kit.parallax_mid,  path)) loaded++;
    snprintf(path, sizeof path, "assets/maps/%s/parallax_near.png", short_name);
    if (kit_load_one(&g_map_kit.parallax_near, path)) loaded++;
    snprintf(path, sizeof path, "assets/maps/%s/tiles.png",         short_name);
    if (kit_load_one(&g_map_kit.tiles,         path)) loaded++;

    if (loaded == 0) {
        LOG_I("map_kit: no per-map assets for '%s' (M4 fallbacks active)", short_name);
    } else {
        LOG_I("map_kit: loaded %d/4 textures for '%s' (par far=%d mid=%d near=%d tiles=%d)",
              loaded, short_name,
              g_map_kit.parallax_far .id != 0,
              g_map_kit.parallax_mid .id != 0,
              g_map_kit.parallax_near.id != 0,
              g_map_kit.tiles        .id != 0);
    }
}
