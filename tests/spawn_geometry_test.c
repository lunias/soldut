/*
 * tests/spawn_geometry_test.c — verify that no spawn point on any
 * shipped map is embedded inside a solid tile or polygon.
 *
 * Spawn coords come from each .lvl's SPWN section (LvlSpawn pos_x /
 * pos_y, world-space px). A spawn that lands inside terrain produces
 * a "mech spawned in the floor / slope" bug — the player materializes
 * inside a tile or polygon and the next tick's collision push-out
 * shoves them sideways unpredictably (or, worse, the bot AI keeps
 * trying to walk into the same trap, looking permanently stuck).
 *
 * For each spawn we sample a rectangle that approximates the mech's
 * standing footprint (pelvis / head / L+R shoulders) and check each
 * sample against:
 *   1. Tile grid via `level_point_solid` (catches tile-fill solids).
 *   2. Polygon broadphase — for every poly whose tile cell the point
 *      sits in, run `point_in_tri` against the triangle. Polys with
 *      kinds SOLID, ICE, and DEADLY block movement; ONE_WAY and
 *      BACKGROUND are non-blocking and skipped.
 *
 * The original validator (tile-only) missed embedded spawns inside
 * authored slope polygons — caught by playtest as "bot stuck on
 * aurora's hill spawn."
 */

#include "../src/arena.h"
#include "../src/level.h"
#include "../src/level_io.h"
#include "../src/log.h"
#include "../src/world.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed_maps = 0;
static int g_failed_spawns = 0;

/* Per-Trooper body extents — the smallest chassis still covers the
 * other four (Heavy is bigger but spawns are usually generous; the
 * tightest constraint is whether the *minimum* body fits).
 *
 * Foot is intentionally NOT sampled: a properly-placed spawn has the
 * foot resting on the floor surface, where `level_point_solid`
 * (tile-only) would always say "yes, in solid" because foot.y sits at
 * the floor tile's top edge. The collision pass push-out handles that
 * marginal case in real play. The samples we do check (pelvis / head /
 * shoulders) are always supposed to be clear air. */
static const float HEAD_DY     = -52.0f;  /* head top above pelvis */
static const float SHOULDER_DX = 15.0f;   /* lateral half-width */

typedef struct { float dx, dy; const char *label; } SamplePoint;

static const SamplePoint SAMPLES[] = {
    {  0.0f,        0.0f,    "pelvis"     },
    {  0.0f,        HEAD_DY, "head"       },
    { -SHOULDER_DX, 0.0f,    "L_shoulder" },
    { +SHOULDER_DX, 0.0f,    "R_shoulder" },
};
static const int SAMPLE_COUNT = (int)(sizeof(SAMPLES) / sizeof(SAMPLES[0]));

static const char *team_name(uint8_t t) {
    switch (t) {
        case 0: return "any";
        case 1: return "red";
        case 2: return "blue";
        default: return "?";
    }
}

/* Same barycentric point-in-triangle as src/mech.c::point_in_tri /
 * src/bot.c::point_in_tri. Duplicated here because both originals are
 * `static` to their compilation units. */
static bool point_in_tri(float px, float py,
                         float ax, float ay,
                         float bx, float by,
                         float cx, float cy)
{
    float d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (fabsf(d) < 1e-6f) return false;
    float u = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d;
    float v = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d;
    return u >= 0.0f && v >= 0.0f && (u + v) <= 1.0f;
}

/* Check polygon broadphase at point P. Returns the kind name of the
 * blocking poly (or NULL if clear). Skips ONE_WAY (you can pass
 * through from below) and BACKGROUND (decoration). SOLID / ICE /
 * DEADLY all block movement and trap a spawn. */
static const char *poly_solid_at(const Level *L, Vec2 P) {
    if (L->poly_count <= 0 || !L->poly_grid_off) return NULL;
    int ts = L->tile_size;
    int tx = (int)(P.x / (float)ts);
    int ty = (int)(P.y / (float)ts);
    if (tx < 0 || tx >= L->width || ty < 0 || ty >= L->height) return NULL;
    int cell = ty * L->width + tx;
    int s = L->poly_grid_off[cell];
    int e = L->poly_grid_off[cell + 1];
    for (int k = s; k < e; ++k) {
        const LvlPoly *poly = &L->polys[L->poly_grid[k]];
        switch ((PolyKind)poly->kind) {
            case POLY_KIND_SOLID:
            case POLY_KIND_ICE:
            case POLY_KIND_DEADLY:
                if (point_in_tri(P.x, P.y,
                                 (float)poly->v_x[0], (float)poly->v_y[0],
                                 (float)poly->v_x[1], (float)poly->v_y[1],
                                 (float)poly->v_x[2], (float)poly->v_y[2])) {
                    switch ((PolyKind)poly->kind) {
                        case POLY_KIND_SOLID:  return "SOLID";
                        case POLY_KIND_ICE:    return "ICE";
                        case POLY_KIND_DEADLY: return "DEADLY";
                        default:               return "?";
                    }
                }
                break;
            default: break;
        }
    }
    return NULL;
}

static bool validate_spawn(const Level *L, const LvlSpawn *s,
                           const char *map_name, int spawn_idx)
{
    bool any_embedded = false;
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        Vec2 p = (Vec2){
            (float)s->pos_x + SAMPLES[i].dx,
            (float)s->pos_y + SAMPLES[i].dy,
        };
        if (level_point_solid(L, p)) {
            fprintf(stderr,
                    "FAIL: %s spawn[%d] team=%s lane=%u — %s sample at "
                    "(%.0f, %.0f) is INSIDE solid TILE\n",
                    map_name, spawn_idx, team_name(s->team), s->lane_hint,
                    SAMPLES[i].label, p.x, p.y);
            any_embedded = true;
            ++g_failed_spawns;
            continue;   /* don't double-report: already failed on tile */
        }
        const char *poly_kind = poly_solid_at(L, p);
        if (poly_kind) {
            fprintf(stderr,
                    "FAIL: %s spawn[%d] team=%s lane=%u — %s sample at "
                    "(%.0f, %.0f) is INSIDE %s polygon\n",
                    map_name, spawn_idx, team_name(s->team), s->lane_hint,
                    SAMPLES[i].label, p.x, p.y, poly_kind);
            any_embedded = true;
            ++g_failed_spawns;
        }
    }
    return any_embedded;
}

static void validate_map(const char *map_path) {
    static World world;
    static Arena arena;
    static bool arena_inited = false;

    /* Reset world's level pointers; the arena holds the storage. */
    memset(&world.level, 0, sizeof(world.level));
    if (!arena_inited) {
        arena_init(&arena, 16 * 1024 * 1024, "spawn_validate");
        arena_inited = true;
    } else {
        arena_reset(&arena);
    }

    LvlResult r = level_load(&world, &arena, map_path);
    if (r != LVL_OK) {
        fprintf(stderr, "SKIP: %s — load failed (%s)\n",
                map_path, level_io_result_str(r));
        return;
    }

    const Level *L = &world.level;
    const char *short_name = strrchr(map_path, '/');
    short_name = short_name ? short_name + 1 : map_path;

    int embedded_in_map = 0;
    for (int i = 0; i < L->spawn_count; ++i) {
        if (validate_spawn(L, &L->spawns[i], short_name, i)) {
            ++embedded_in_map;
        }
    }
    if (embedded_in_map > 0) {
        fprintf(stdout,
                "FAIL: %s — %d / %d spawns embedded in geometry\n",
                short_name, embedded_in_map, L->spawn_count);
        ++g_failed_maps;
    } else {
        fprintf(stdout,
                "PASS: %s — %d spawns clear (%d body samples each = %d checks)\n",
                short_name, L->spawn_count, SAMPLE_COUNT,
                L->spawn_count * SAMPLE_COUNT);
    }
}

int main(void) {
    /* All shipped + cooked maps. Order doesn't matter; failures are
     * per-map. */
    const char *maps[] = {
        "assets/maps/foundry.lvl",
        "assets/maps/slipstream.lvl",
        "assets/maps/reactor.lvl",
        "assets/maps/concourse.lvl",
        "assets/maps/catwalk.lvl",
        "assets/maps/aurora.lvl",
        "assets/maps/crossfire.lvl",
        "assets/maps/citadel.lvl",
        "assets/maps/grapple_test.lvl",
        "assets/maps/ctf_test.lvl",
        NULL,
    };

    for (int i = 0; maps[i]; ++i) {
        validate_map(maps[i]);
    }

    if (g_failed_spawns > 0) {
        fprintf(stderr,
                "\nspawn_geometry_test: %d spawn body-samples embedded "
                "across %d map(s) — FAIL\n",
                g_failed_spawns, g_failed_maps);
        return 1;
    }
    fprintf(stdout, "\nspawn_geometry_test: all spawn body samples clear\n");
    return 0;
}
