#include "bot.h"

#include "arena.h"
#include "ctf.h"
#include "game.h"
#include "hash.h"
#include "level.h"
#include "log.h"
#include "match.h"
#include "mech.h"
#include "physics.h"
#include "pickup.h"
#include "weapons.h"
#include "world.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Bot AI — Quake III-style layered classical AI on 2D Verlet mechs.
 * See documents/13-bot-ai.md for the spec.
 *
 * Layout of this file (~one canonical seam per section):
 *
 *   1. Tunables + per-tier personality table
 *   2. Nav graph (BotNav data structures, build, A*)
 *   3. World helpers (pelvis pos, LOS, nearest-enemy)
 *   4. Strategy layer (utility scorer + goal pick)
 *   5. Tactic layer (per-goal switch filling BotWants)
 *   6. Motor (BotWants → ClientInput + aim slew + jitter)
 *   7. Public API (init, attach, detach, step)
 */

/* ---- 1. Tunables ---------------------------------------------------- */

#define BOT_TICK_HZ              60
#define BOT_TICK_DT              (1.0f / 60.0f)

#define BOT_STRATEGY_INTERVAL    6      /* every 6 ticks = ~10 Hz */
#define BOT_NAV_MAX_NODES        512
#define BOT_NAV_MAX_REACH        4096   /* ~8 per node */
#define BOT_NAV_FLOOR_SAMPLE_T   3      /* one column every 3 tiles (96 px) — denser than the
                                         * documented 128 px so big maps still have a useful
                                         * node count without blowing past BOT_NAV_MAX_NODES */
#define BOT_NAV_REACH_MAX_PX     280.0f /* candidate-pair pruning radius */
#define BOT_NAV_JUMP_DH_MAX_PX   40.0f  /* max vertical rise a JUMP reach can cover */
#define BOT_NAV_JUMP_DX_MAX_PX   160.0f
#define BOT_NAV_GRAPPLE_MAX_PX   300.0f
#define BOT_PATH_REPLAN_PX       96.0f
#define BOT_STUCK_TICKS          (BOT_TICK_HZ * 1)

#define BOT_RUN_SPEED_PXS        280.0f
#define BOT_JET_THRUST_PXS2      2200.0f

#define BOT_AWARENESS_LOS_STEPS  10

/* Hysteresis: stick with the current goal unless a candidate scores
 * more than (current * 1.10). */
#define BOT_GOAL_HYST_GAIN       1.10f

/* ---- Per-tier personality table ------------------------------------- */

static const BotPersonality g_bot_tier_table[BOT_TIER_COUNT] = {
    [BOT_TIER_RECRUIT] = {
        .tier                  = BOT_TIER_RECRUIT,
        .aim_jitter_rad        = 0.18f,     /* ≈10° */
        .aim_lead_frac         = 0.20f,
        .aim_slew_rad_per_tick = 0.04f,
        .reaction_ticks        = 30.0f,     /* 500 ms */
        .awareness_radius_px   = 500.0f,
        .pickup_priority       = 0.20f,
        .flag_priority         = 0.10f,
        .grapple_priority      = 0.00f,
        .aggression            = 0.30f,
        .knows_full_map        = 0,
        .uses_powerups         = 0,
    },
    [BOT_TIER_VETERAN] = {
        .tier                  = BOT_TIER_VETERAN,
        .aim_jitter_rad        = 0.08f,     /* ≈4.5° */
        .aim_lead_frac         = 0.55f,
        .aim_slew_rad_per_tick = 0.10f,
        .reaction_ticks        = 9.0f,      /* ~150 ms */
        .awareness_radius_px   = 900.0f,
        .pickup_priority       = 0.55f,
        .flag_priority         = 0.50f,
        .grapple_priority      = 0.15f,
        .aggression            = 0.55f,
        .knows_full_map        = 1,
        .uses_powerups         = 0,
    },
    [BOT_TIER_ELITE] = {
        .tier                  = BOT_TIER_ELITE,
        .aim_jitter_rad        = 0.030f,    /* ≈1.7° */
        .aim_lead_frac         = 0.80f,
        .aim_slew_rad_per_tick = 0.18f,
        .reaction_ticks        = 4.0f,
        .awareness_radius_px   = 1300.0f,
        .pickup_priority       = 0.80f,
        .flag_priority         = 0.75f,
        .grapple_priority      = 0.45f,
        .aggression            = 0.75f,
        .knows_full_map        = 1,
        .uses_powerups         = 1,
    },
    [BOT_TIER_CHAMPION] = {
        .tier                  = BOT_TIER_CHAMPION,
        .aim_jitter_rad        = 0.005f,    /* ≈0.3° */
        .aim_lead_frac         = 1.00f,
        .aim_slew_rad_per_tick = 0.32f,
        .reaction_ticks        = 2.0f,
        .awareness_radius_px   = 100000.0f, /* effectively unlimited */
        .pickup_priority       = 1.00f,
        .flag_priority         = 1.00f,
        .grapple_priority      = 0.85f,
        .aggression            = 0.95f,
        .knows_full_map        = 1,
        .uses_powerups         = 1,
    },
};

BotPersonality bot_personality_for_tier(BotTier tier) {
    if (tier < 0 || tier >= BOT_TIER_COUNT) tier = BOT_TIER_VETERAN;
    return g_bot_tier_table[tier];
}

const char *bot_tier_name(BotTier tier) {
    switch (tier) {
        case BOT_TIER_RECRUIT:  return "Recruit";
        case BOT_TIER_VETERAN:  return "Veteran";
        case BOT_TIER_ELITE:    return "Elite";
        case BOT_TIER_CHAMPION: return "Champion";
        default:                return "Veteran";
    }
}

BotTier bot_tier_from_name(const char *s) {
    if (!s || !*s) return BOT_TIER_VETERAN;
    if (s[0] == 'r' || s[0] == 'R') return BOT_TIER_RECRUIT;
    if (s[0] == 'v' || s[0] == 'V') return BOT_TIER_VETERAN;
    if (s[0] == 'e' || s[0] == 'E') return BOT_TIER_ELITE;
    if (s[0] == 'c' || s[0] == 'C') return BOT_TIER_CHAMPION;
    return BOT_TIER_VETERAN;
}

const char *bot_goal_name(BotGoal g) {
    switch (g) {
        case BOT_GOAL_IDLE:          return "idle";
        case BOT_GOAL_ENGAGE:        return "engage";
        case BOT_GOAL_REPOSITION:    return "reposition";
        case BOT_GOAL_PURSUE_PICKUP: return "pursue_pickup";
        case BOT_GOAL_GRAB_FLAG:     return "grab_flag";
        case BOT_GOAL_RETURN_FLAG:   return "return_flag";
        case BOT_GOAL_DEFEND_FLAG:   return "defend_flag";
        case BOT_GOAL_CHASE_CARRIER: return "chase_carrier";
        case BOT_GOAL_CAPTURE:       return "capture";
        case BOT_GOAL_RETREAT:       return "retreat";
        default:                      return "?";
    }
}

/* Loadout variety. Mix a handful of chassis + primaries by index so a
 * bot fight looks varied. */
void bot_default_loadout_for_tier(int bot_index, BotTier tier,
                                  MechLoadout *out)
{
    if (!out) return;
    *out = mech_default_loadout();
    static const int chassis[] = {
        CHASSIS_TROOPER, CHASSIS_SCOUT, CHASSIS_HEAVY,
        CHASSIS_SNIPER, CHASSIS_ENGINEER
    };
    static const int primaries[] = {
        WEAPON_PULSE_RIFLE, WEAPON_PLASMA_SMG, WEAPON_AUTO_CANNON,
        WEAPON_RIOT_CANNON, WEAPON_PLASMA_CANNON,
    };
    int i = (bot_index < 0) ? 0 : bot_index;
    out->chassis_id   = chassis  [i % (int)(sizeof chassis  / sizeof chassis[0])];
    out->primary_id   = primaries[i % (int)(sizeof primaries/ sizeof primaries[0])];
    /* Higher tiers more likely to carry grenades + grapple — they know
     * how to use both. Recruits stick with the sidearm. */
    if (tier >= BOT_TIER_VETERAN && (i & 3) == 0) {
        out->secondary_id = WEAPON_FRAG_GRENADES;
    } else if (tier >= BOT_TIER_ELITE && (i & 3) == 2) {
        out->secondary_id = WEAPON_GRAPPLING_HOOK;
    } else {
        out->secondary_id = WEAPON_SIDEARM;
    }
    out->armor_id   = ARMOR_LIGHT;
    out->jetpack_id = JET_STANDARD;
}

const char *bot_name_for_index(int bot_index, BotTier tier,
                               char *buf, int cap)
{
    if (!buf || cap <= 0) return "";
    /* Just the slot number — the lobby UI already paints a per-tier
     * chip next to the name, so duplicating the tier here just makes
     * the row noisy. Stays under LOBBY_NAME_BYTES (24). */
    (void)tier;
    snprintf(buf, (size_t)cap, "Bot-%d", bot_index + 1);
    buf[cap - 1] = '\0';
    return buf;
}

/* ---- 2. Nav graph --------------------------------------------------- */

typedef enum {
    BOT_REACH_WALK    = 0,
    BOT_REACH_JUMP    = 1,
    BOT_REACH_FALL    = 2,
    BOT_REACH_JET     = 3,
    BOT_REACH_GRAPPLE = 4,
    BOT_REACH_ONE_WAY = 5,
} BotReachKind;

#define BOT_NODE_F_ON_FLOOR    (1u << 0)
#define BOT_NODE_F_ON_PLATFORM (1u << 1)
#define BOT_NODE_F_SPAWN       (1u << 2)
#define BOT_NODE_F_PICKUP      (1u << 3)
#define BOT_NODE_F_FLAG        (1u << 4)
#define BOT_NODE_F_SLOPE_TOP   (1u << 5)

typedef struct BotNavNode {
    Vec2     pos;
    uint16_t flags;
    uint8_t  pickup_kind;
    uint8_t  pickup_variant;
    int16_t  pickup_spawner_idx;  /* index into world.pickups[] when PICKUP */
    int8_t   flag_team;           /* 1/2/-1 */
    uint8_t  pad0;
    uint16_t reach_first;
    uint16_t reach_count;
} BotNavNode;

typedef struct BotNavReach {
    uint16_t from_node;
    uint16_t to_node;
    uint8_t  kind;                 /* BotReachKind */
    uint8_t  required_fuel_q8;
    uint16_t cost_ms;
} BotNavReach;

struct BotNav {
    BotNavNode  nodes  [BOT_NAV_MAX_NODES];
    int         node_count;
    BotNavReach reaches[BOT_NAV_MAX_REACH];
    int         reach_count;
    float       level_w_px;
    float       level_h_px;
    int         tile_size;
};

/* Per-cell spatial-hash bucket for nearest-node queries. The grid is a
 * fixed 64×32 cells regardless of level size — coarse enough that each
 * cell holds <= 12 nodes on a 200×100 map, fine enough that lookup is
 * O(1+k) per query. */
#define BOT_NAV_GRID_W 64
#define BOT_NAV_GRID_H 32
#define BOT_NAV_GRID_CELL_MAX 16

typedef struct {
    int16_t  node_ids[BOT_NAV_GRID_CELL_MAX];
    uint8_t  count;
} BotNavGridCell;

/* Static so we don't pay an arena cost; one is enough since only one
 * level is loaded at a time. */
static BotNavGridCell s_nav_grid[BOT_NAV_GRID_H][BOT_NAV_GRID_W];

static void nav_grid_clear(void) {
    memset(s_nav_grid, 0, sizeof s_nav_grid);
}

static void nav_grid_for_pos(const struct BotNav *nv, Vec2 p,
                             int *out_gx, int *out_gy)
{
    if (nv->level_w_px <= 0.0f || nv->level_h_px <= 0.0f) {
        *out_gx = 0; *out_gy = 0; return;
    }
    int gx = (int)((p.x / nv->level_w_px) * (float)BOT_NAV_GRID_W);
    int gy = (int)((p.y / nv->level_h_px) * (float)BOT_NAV_GRID_H);
    if (gx < 0) gx = 0;
    if (gx >= BOT_NAV_GRID_W) gx = BOT_NAV_GRID_W - 1;
    if (gy < 0) gy = 0;
    if (gy >= BOT_NAV_GRID_H) gy = BOT_NAV_GRID_H - 1;
    *out_gx = gx;
    *out_gy = gy;
}

static void nav_grid_add(const struct BotNav *nv, int node_id) {
    int gx, gy;
    nav_grid_for_pos(nv, nv->nodes[node_id].pos, &gx, &gy);
    BotNavGridCell *c = &s_nav_grid[gy][gx];
    if (c->count < BOT_NAV_GRID_CELL_MAX) {
        c->node_ids[c->count++] = (int16_t)node_id;
    }
}

/* Find closest node to a world-space point. Walks a 3x3 cell window
 * (the supplied point + 8 neighbors) so cell-boundary cases stay
 * correct. Returns -1 if no node within max_dist_px. */
static int nav_nearest_node(const struct BotNav *nv, Vec2 p,
                            float max_dist_px, uint16_t require_flags)
{
    if (!nv || nv->node_count == 0) return -1;
    int gx, gy;
    nav_grid_for_pos(nv, p, &gx, &gy);
    int best = -1;
    float best_d2 = max_dist_px * max_dist_px;
    for (int dy = -1; dy <= 1; ++dy) {
        int yy = gy + dy;
        if (yy < 0 || yy >= BOT_NAV_GRID_H) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            int xx = gx + dx;
            if (xx < 0 || xx >= BOT_NAV_GRID_W) continue;
            const BotNavGridCell *c = &s_nav_grid[yy][xx];
            for (int i = 0; i < c->count; ++i) {
                int id = c->node_ids[i];
                const BotNavNode *n = &nv->nodes[id];
                if (require_flags && !(n->flags & require_flags)) continue;
                float dxp = n->pos.x - p.x;
                float dyp = n->pos.y - p.y;
                float d2  = dxp*dxp + dyp*dyp;
                if (d2 < best_d2) { best_d2 = d2; best = id; }
            }
        }
    }
    return best;
}

/* ---- Tile + polygon helpers ----------------------------------------- */

static bool tile_is_solid(const Level *L, int tx, int ty) {
    return (level_flags_at(L, tx, ty) & TILE_F_SOLID) != 0;
}

static bool tile_is_floor_top(const Level *L, int tx, int ty) {
    if (!tile_is_solid(L, tx, ty)) return false;
    if (ty == 0) return false;
    if (tile_is_solid(L, tx, ty - 1)) return false;
    return true;
}

/* Sample world-space columns and emit a nav node at the top of every
 * walkable surface. Catches both tile floors AND polygon-built floors
 * (slopes, bowls, platforms). For each column at FLOOR_SAMPLE_T tile
 * intervals, scan top-down; the first cell whose center is NOT solid
 * but whose cell *just below* IS solid is the surface top — place a
 * node there. Continues scanning to find platform tops too (a level
 * has multiple walkable layers stacked vertically).
 *
 * Solidity check uses the polygon broadphase via `level_point_solid`
 * for tiles, plus a polygon point-in-triangle test for free polys —
 * `level_ray_hits` already integrates both. We use a tiny vertical
 * ray from one px above to one px below the candidate row to detect
 * "this row's top edge is a surface." */
/* Sign of the cross product (B-A) × (P-A). Used by point_in_tri. */
static inline float tri_side(float ax, float ay, float bx, float by,
                             float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static bool point_in_tri(float px, float py,
                         float ax, float ay, float bx, float by,
                         float cx, float cy)
{
    float d1 = tri_side(ax, ay, bx, by, px, py);
    float d2 = tri_side(bx, by, cx, cy, px, py);
    float d3 = tri_side(cx, cy, ax, ay, px, py);
    bool has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(has_neg && has_pos);
}

/* True iff (wx, wy) is inside a SOLID tile or a SOLID/ICE polygon.
 * Treats OOB explicitly as "not solid" so the column sweep can start
 * above the world without the OOB-tile sentinel firing. */
static bool point_solid(const Level *L, float wx, float wy) {
    int tx = (int)(wx / (float)L->tile_size);
    int ty = (int)(wy / (float)L->tile_size);
    if (tx >= 0 && tx < L->width && ty >= 0 && ty < L->height) {
        uint16_t flags = L->tiles[ty * L->width + tx].flags;
        if (flags & TILE_F_SOLID) return true;
    }
    /* Triangle list scan — small enough on every M5 map (<= a few
     * hundred polys) to walk linearly. ICE counts as solid for nav
     * (you can stand on ice). BACKGROUND / DEADLY / ONE_WAY skip. */
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *p = &L->polys[i];
        if (p->kind != POLY_KIND_SOLID && p->kind != POLY_KIND_ICE) continue;
        if (point_in_tri(wx, wy,
                         (float)p->v_x[0], (float)p->v_y[0],
                         (float)p->v_x[1], (float)p->v_y[1],
                         (float)p->v_x[2], (float)p->v_y[2])) {
            return true;
        }
    }
    return false;
}

static void build_floor_nodes(struct BotNav *nv, const Level *L) {
    const int ts = L->tile_size;
    const int sample_step_px = ts * BOT_NAV_FLOOR_SAMPLE_T;
    const int W_px = L->width  * ts;
    const int H_px = L->height * ts;
    /* Start one half-step in so columns aren't right against the
     * outer wall, and step in tile-pixel units. */
    for (int wx = sample_step_px / 2; wx < W_px; wx += sample_step_px) {
        /* Top-down sweep. We pretend the sweep starts above the world
         * in clear air; the first air→solid transition is a surface
         * top, and the first solid→air transition resets so the next
         * surface (a platform top, etc.) is also emitted. Lets us
         * sample columns that start on a wall and dive down past a
         * platform opening into the floor below. */
        bool prev_solid = false;
        int  last_node_y = -10000;
        for (int wy = 0; wy < H_px; wy += 8) {
            bool here = point_solid(L, (float)wx, (float)wy);
            if (!prev_solid && here && wy > 8) {
                int node_y = wy - 2;
                if (node_y - last_node_y >= 64) {
                    if (nv->node_count >= BOT_NAV_MAX_NODES) return;
                    /* Minimal headroom check: ensure there's at least
                     * 16 px of air directly above (so we're not placing
                     * a node inside a 1-tile crack). The full body
                     * height is only required for path edges, which the
                     * corridor-clear test handles. */
                    if (!point_solid(L, (float)wx, (float)(wy - 16))) {
                        BotNavNode *n = &nv->nodes[nv->node_count++];
                        memset(n, 0, sizeof *n);
                        n->pos = (Vec2){ (float)wx, (float)node_y };
                        n->flags = BOT_NODE_F_ON_FLOOR;
                        n->pickup_spawner_idx = -1;
                        n->flag_team = -1;
                        last_node_y = node_y;
                    }
                }
            }
            prev_solid = here;
        }
    }
}

static void build_spawn_nodes(struct BotNav *nv, const Level *L) {
    for (int i = 0; i < L->spawn_count; ++i) {
        if (nv->node_count >= BOT_NAV_MAX_NODES) return;
        const LvlSpawn *s = &L->spawns[i];
        BotNavNode *n = &nv->nodes[nv->node_count++];
        memset(n, 0, sizeof *n);
        n->pos = (Vec2){ (float)s->pos_x, (float)s->pos_y };
        n->flags = BOT_NODE_F_SPAWN;
        n->pickup_spawner_idx = -1;
        n->flag_team = -1;
    }
}

static void build_pickup_nodes(struct BotNav *nv, const Level *L) {
    for (int i = 0; i < L->pickup_count; ++i) {
        if (nv->node_count >= BOT_NAV_MAX_NODES) return;
        const LvlPickup *p = &L->pickups[i];
        /* PICKUP_PRACTICE_DUMMY is consumed at round start into a dummy
         * mech — never a real pickup. Skip. */
        if (p->category == PICKUP_PRACTICE_DUMMY) continue;
        BotNavNode *n = &nv->nodes[nv->node_count++];
        memset(n, 0, sizeof *n);
        n->pos = (Vec2){ (float)p->pos_x, (float)p->pos_y };
        n->flags = BOT_NODE_F_PICKUP;
        n->pickup_kind        = p->category;
        n->pickup_variant     = p->variant;
        n->pickup_spawner_idx = (int16_t)i;
        n->flag_team          = -1;
    }
}

static void build_flag_nodes(struct BotNav *nv, const Level *L) {
    for (int i = 0; i < L->flag_count; ++i) {
        if (nv->node_count >= BOT_NAV_MAX_NODES) return;
        const LvlFlag *f = &L->flags[i];
        BotNavNode *n = &nv->nodes[nv->node_count++];
        memset(n, 0, sizeof *n);
        n->pos = (Vec2){ (float)f->pos_x, (float)f->pos_y };
        n->flags = BOT_NODE_F_FLAG;
        n->pickup_spawner_idx = -1;
        n->flag_team = (int8_t)f->team;
    }
}

/* ---- Reachability tests --------------------------------------------- */

/* Vertical channel free of solid tiles between (tx, y0) and (tx, y1). */
static bool vertical_clear(const Level *L, int tx, int ty_top, int ty_bot) {
    if (ty_top > ty_bot) { int t = ty_top; ty_top = ty_bot; ty_bot = t; }
    for (int ty = ty_top; ty <= ty_bot; ++ty) {
        if (tile_is_solid(L, tx, ty)) return false;
    }
    return true;
}

/* Horizontal channel free between (tx0..tx1, ty). */
static bool horizontal_clear(const Level *L, int ty, int tx0, int tx1) {
    if (tx0 > tx1) { int t = tx0; tx0 = tx1; tx1 = t; }
    for (int tx = tx0; tx <= tx1; ++tx) {
        if (tile_is_solid(L, tx, ty)) return false;
    }
    return true;
}

/* Test that the body's path is clear of geometry — tiles AND polys.
 * Two rays at pelvis-height and head-height (-40 px) — we deliberately
 * do NOT sweep at foot level: nav nodes sit ~2 px above a surface, and
 * a foot ray would always clip into the surface itself, breaking every
 * cross-floor WALK reach. The pelvis ray catches cover columns + walls
 * at body height; the head ray catches low overhangs that would knock
 * the mech off a jet. */
static bool body_corridor_clear(const Level *L, Vec2 from, Vec2 to) {
    float t;
    Vec2 offs[2] = { (Vec2){0, -40}, (Vec2){0, -8} };
    for (int i = 0; i < 2; ++i) {
        Vec2 a = (Vec2){ from.x + offs[i].x, from.y + offs[i].y };
        Vec2 b = (Vec2){ to.x   + offs[i].x, to.y   + offs[i].y };
        if (level_ray_hits(L, a, b, &t)) return false;
    }
    return true;
}

/* Returns BotReachKind + cost (ms) when a reachability from `a` to `b`
 * exists for our mech, or -1 (no reach). */
static int classify_reach(const Level *L, const BotNavNode *a, const BotNavNode *b,
                          uint16_t *out_cost_ms, uint8_t *out_fuel_q8)
{
    *out_cost_ms = 0;
    *out_fuel_q8 = 0;

    float dx = b->pos.x - a->pos.x;
    float dy = b->pos.y - a->pos.y;
    float dist = sqrtf(dx*dx + dy*dy);
    if (dist > BOT_NAV_REACH_MAX_PX) {
        /* Only GRAPPLE can reach beyond the standard prune, and only up
         * to GRAPPLE_MAX_REST_LEN. */
        if (dist <= BOT_NAV_GRAPPLE_MAX_PX) {
            float t;
            if (!level_ray_hits(L, a->pos, b->pos, &t)) {
                *out_cost_ms = (uint16_t)(dist * 1.05f + 100.0f);
                return BOT_REACH_GRAPPLE;
            }
        }
        return -1;
    }

    /* WALK: same Y (within ±8 px), body corridor clear, both on floor. */
    if ((a->flags & BOT_NODE_F_ON_FLOOR) && (b->flags & BOT_NODE_F_ON_FLOOR) &&
        fabsf(dy) <= 8.0f)
    {
        if (body_corridor_clear(L, a->pos, b->pos)) {
            *out_cost_ms = (uint16_t)(dist * 1000.0f / BOT_RUN_SPEED_PXS);
            return BOT_REACH_WALK;
        }
    }

    /* JUMP: small rise, modest horizontal gap. */
    if (dy <= 0.0f && -dy <= BOT_NAV_JUMP_DH_MAX_PX &&
        fabsf(dx) <= BOT_NAV_JUMP_DX_MAX_PX)
    {
        if (body_corridor_clear(L, a->pos, b->pos)) {
            *out_cost_ms = (uint16_t)(dist * 1.1f * 1000.0f / BOT_RUN_SPEED_PXS);
            return BOT_REACH_JUMP;
        }
    }

    /* FALL: target below source, vertical channel mostly free. */
    if (dy > 32.0f) {
        if (body_corridor_clear(L, a->pos, b->pos)) {
            *out_cost_ms = (uint16_t)(fabsf(dx) * 0.9f * 1000.0f / BOT_RUN_SPEED_PXS);
            return BOT_REACH_FALL;
        }
    }

    /* JET: target above. Fuel ∝ rise — required_fuel = clamp((rise/200), 0..1). */
    if (dy < -BOT_NAV_JUMP_DH_MAX_PX && dy >= -200.0f) {
        if (body_corridor_clear(L, a->pos, b->pos)) {
            float rise = -dy;
            float fuel_frac = rise / 200.0f;
            if (fuel_frac > 1.0f) fuel_frac = 1.0f;
            *out_fuel_q8 = (uint8_t)(fuel_frac * 255.0f);
            *out_cost_ms = (uint16_t)(dist * 1.4f * 1000.0f / BOT_RUN_SPEED_PXS);
            return BOT_REACH_JET;
        }
    }

    /* GRAPPLE: long-range LOS, only for big vertical reach. */
    if (-dy > 80.0f && dist <= BOT_NAV_GRAPPLE_MAX_PX) {
        float t;
        if (!level_ray_hits(L, a->pos, b->pos, &t)) {
            *out_cost_ms = (uint16_t)(dist * 1.05f + 100.0f);
            return BOT_REACH_GRAPPLE;
        }
    }

    return -1;
}

static void build_reachabilities(struct BotNav *nv, const Level *L) {
    int N = nv->node_count;
    /* For each `from` node, scan candidates and emit reaches into the
     * shared `reaches` array, marking `reach_first` / `reach_count`. */
    nv->reach_count = 0;
    for (int i = 0; i < N; ++i) {
        BotNavNode *a = &nv->nodes[i];
        a->reach_first = (uint16_t)nv->reach_count;
        a->reach_count = 0;
        for (int j = 0; j < N; ++j) {
            if (i == j) continue;
            const BotNavNode *b = &nv->nodes[j];
            /* Cheap prune by Manhattan distance. */
            float manh = fabsf(b->pos.x - a->pos.x) + fabsf(b->pos.y - a->pos.y);
            if (manh > BOT_NAV_GRAPPLE_MAX_PX * 1.5f) continue;

            uint16_t cost_ms = 0;
            uint8_t  fuel_q8 = 0;
            int kind = classify_reach(L, a, b, &cost_ms, &fuel_q8);
            if (kind < 0) continue;
            if (nv->reach_count >= BOT_NAV_MAX_REACH) {
                LOG_W("bot_nav: reach pool full (%d) — truncating",
                      BOT_NAV_MAX_REACH);
                return;
            }
            BotNavReach *r = &nv->reaches[nv->reach_count++];
            r->from_node        = (uint16_t)i;
            r->to_node          = (uint16_t)j;
            r->kind             = (uint8_t)kind;
            r->required_fuel_q8 = fuel_q8;
            r->cost_ms          = cost_ms ? cost_ms : 1;
            a->reach_count++;
        }
    }
}

/* ---- A* ------------------------------------------------------------- */

/* Open-set entries use a flat array with linear-scan find-min. With
 * BOT_NAV_MAX_NODES = 512 and typical pathlen ~30, expansion cost is
 * dominated by the constraint solver. Worst-case 500*500*8 = 2 Mops
 * per A* run — still <1 ms. */

typedef struct {
    int16_t  came_from[BOT_NAV_MAX_NODES];
    uint16_t g_score  [BOT_NAV_MAX_NODES];   /* ms */
    uint16_t f_score  [BOT_NAV_MAX_NODES];
    uint8_t  in_open  [BOT_NAV_MAX_NODES];
    uint8_t  closed   [BOT_NAV_MAX_NODES];
    /* Reach kind used to arrive at `i` (from came_from[i]). */
    uint8_t  reach_kind[BOT_NAV_MAX_NODES];
} BotPathScratch;

static uint16_t heuristic_ms(Vec2 a, Vec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float d = sqrtf(dx*dx + dy*dy);
    float ms = d * 1000.0f / BOT_RUN_SPEED_PXS;
    if (ms > 65000.0f) ms = 65000.0f;
    return (uint16_t)ms;
}

static int astar_find_min(const BotPathScratch *s, int N) {
    int best = -1;
    uint16_t best_f = 0xFFFFu;
    for (int i = 0; i < N; ++i) {
        if (!s->in_open[i]) continue;
        if (s->f_score[i] < best_f) { best_f = s->f_score[i]; best = i; }
    }
    return best;
}

/* Plan a path. Writes node ids into mind->path / path_len / path_step.
 * Returns true on success; false if no path exists. */
static bool bot_plan_path(BotMind *mind, const struct BotNav *nv,
                          int start_node, int goal_node)
{
    if (!nv || start_node < 0 || goal_node < 0) return false;
    if (start_node == goal_node) {
        mind->path[0] = (int16_t)goal_node;
        mind->path_len = 1;
        mind->path_step = 0;
        mind->path_reach_kind = BOT_REACH_WALK;
        return true;
    }
    BotPathScratch s;
    memset(&s, 0, sizeof s);
    int N = nv->node_count;
    for (int i = 0; i < N; ++i) {
        s.came_from[i] = -1;
        s.g_score[i]   = 0xFFFFu;
        s.f_score[i]   = 0xFFFFu;
    }
    s.g_score[start_node] = 0;
    s.f_score[start_node] = heuristic_ms(nv->nodes[start_node].pos,
                                          nv->nodes[goal_node].pos);
    s.in_open[start_node] = 1;

    /* Capped expansion budget keeps the worst case bounded. Bumped
     * 2N→4N so big maps (Citadel, Crossfire) reliably find a path
     * across the whole graph. With 200 nodes per nav graph the cap
     * is 800 expansions — at ~50 ns each, that's 40 µs of A* time. */
    int max_expansions = N * 4;
    int found_goal     = 0;
    while (max_expansions-- > 0) {
        int cur = astar_find_min(&s, N);
        if (cur < 0) break;
        if (cur == goal_node) { found_goal = 1; break; }
        s.in_open[cur] = 0;
        s.closed[cur]  = 1;
        const BotNavNode *cn = &nv->nodes[cur];
        for (int k = 0; k < cn->reach_count; ++k) {
            const BotNavReach *r = &nv->reaches[cn->reach_first + k];
            int next = r->to_node;
            if (s.closed[next]) continue;
            uint32_t tentative = (uint32_t)s.g_score[cur] + (uint32_t)r->cost_ms;
            if (tentative >= 0xFFFFu) tentative = 0xFFFEu;
            if (tentative < s.g_score[next]) {
                s.came_from[next] = (int16_t)cur;
                s.g_score [next] = (uint16_t)tentative;
                uint16_t h = heuristic_ms(nv->nodes[next].pos,
                                           nv->nodes[goal_node].pos);
                uint32_t f = tentative + h;
                if (f >= 0xFFFFu) f = 0xFFFEu;
                s.f_score [next] = (uint16_t)f;
                s.reach_kind[next] = r->kind;
                s.in_open[next] = 1;
            }
        }
    }
    if (!found_goal) return false;

    /* Walk came_from backward, write into mind->path in forward order
     * (start → goal). Truncate to BOT_MAX_PATH_LEN — A* finds the full
     * path but we only consume the prefix; the bot replans when the
     * prefix runs out. */
    int16_t rev[BOT_NAV_MAX_NODES];
    int rev_n = 0;
    int cur = goal_node;
    while (cur >= 0 && rev_n < BOT_NAV_MAX_NODES) {
        rev[rev_n++] = (int16_t)cur;
        if (cur == start_node) break;
        cur = s.came_from[cur];
    }
    if (rev_n <= 0) return false;
    /* Reverse, cap at BOT_MAX_PATH_LEN. Skip the very first entry
     * (==start_node) so path[0] is the bot's next destination. */
    int forward_n = rev_n - 1;  /* skip start */
    if (forward_n > BOT_MAX_PATH_LEN) forward_n = BOT_MAX_PATH_LEN;
    if (forward_n <= 0) {
        mind->path[0] = (int16_t)goal_node;
        mind->path_len = 1;
        mind->path_step = 0;
        mind->path_reach_kind = BOT_REACH_WALK;
        return true;
    }
    for (int i = 0; i < forward_n; ++i) {
        mind->path[i] = rev[rev_n - 2 - i];
    }
    mind->path_len  = (uint8_t)forward_n;
    mind->path_step = 0;
    mind->path_reach_kind = s.reach_kind[mind->path[0]];
    return true;
}

/* ---- 3. World helpers ----------------------------------------------- */

static inline int mech_pelvis_idx(const World *w, int mid) {
    return w->mechs[mid].particle_base + PART_PELVIS;
}

static inline Vec2 mech_pelvis_pos(const World *w, int mid) {
    int p = mech_pelvis_idx(w, mid);
    return (Vec2){ w->particles.pos_x[p], w->particles.pos_y[p] };
}

static inline Vec2 mech_pelvis_vel(const World *w, int mid) {
    int p = mech_pelvis_idx(w, mid);
    return (Vec2){
        (w->particles.pos_x[p] - w->particles.prev_x[p]) * (float)BOT_TICK_HZ,
        (w->particles.pos_y[p] - w->particles.prev_y[p]) * (float)BOT_TICK_HZ,
    };
}

static bool los_clear(const World *w, Vec2 a, Vec2 b) {
    float t;
    return !level_ray_hits(&w->level, a, b, &t);
}

static int find_nearest_enemy(const World *w, int mid, float max_range,
                              MatchModeId mode)
{
    const Mech *me = &w->mechs[mid];
    if (!me->alive) return -1;
    Vec2 mp = mech_pelvis_pos(w, mid);
    int best = -1;
    float best_d2 = max_range * max_range;
    for (int i = 0; i < w->mech_count; ++i) {
        if (i == mid) continue;
        const Mech *o = &w->mechs[i];
        if (!o->alive) continue;
        if (o->is_dummy) continue;
        /* Team filtering — same team is not an enemy unless FFA. */
        if (mode != MATCH_MODE_FFA && o->team == me->team) continue;
        Vec2 op = mech_pelvis_pos(w, i);
        float dx = op.x - mp.x, dy = op.y - mp.y;
        float d2 = dx*dx + dy*dy;
        if (d2 >= best_d2) continue;
        if (!los_clear(w, mp, op)) continue;
        best_d2 = d2;
        best = i;
    }
    return best;
}

/* ---- 4. Strategy layer ---------------------------------------------- */

typedef struct {
    BotGoal goal;
    int     target_mech;
    int     target_node;
} GoalPick;

static float health_fraction(const Mech *m) {
    if (m->health_max <= 0.0f) return 0.0f;
    float f = m->health / m->health_max;
    if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
    return f;
}

/* `pickup_need_factor` — 0..1 weight on how much the bot needs a
 * particular pickup right now. Out-of-cooldown pickups are scored 0. */
static float pickup_need_factor(const Mech *m, const PickupSpawner *s) {
    if (s->state != PICKUP_STATE_AVAILABLE) return 0.0f;
    switch (s->kind) {
        case PICKUP_HEALTH:
            return 1.0f - health_fraction(m);
        case PICKUP_ARMOR:
            if (m->armor_hp_max <= 0.0f) return 0.6f;
            return 1.0f - (m->armor_hp / m->armor_hp_max);
        case PICKUP_JET_FUEL:
            if (m->fuel_max <= 0.0f) return 0.4f;
            return 1.0f - (m->fuel / m->fuel_max);
        case PICKUP_AMMO_PRIMARY:
        case PICKUP_AMMO_SECONDARY:
            return (m->ammo <= 4) ? 0.9f : 0.3f;
        case PICKUP_WEAPON:
            return 0.5f;
        case PICKUP_POWERUP:
            return 0.8f;
        case PICKUP_REPAIR_PACK:
            return 1.0f - health_fraction(m);
        default:
            return 0.3f;
    }
}

/* Walk the live pickup pool and pick the most attractive AVAILABLE
 * spawner. Returns the spawner index, or -1. Also picks the nearest
 * nav node colocated with that spawner. */
static int pick_best_pickup(const World *w, const BotMind *mind,
                            const struct BotNav *nv, int mid,
                            int *out_node)
{
    const Mech *me = &w->mechs[mid];
    Vec2 mp = mech_pelvis_pos(w, mid);
    int   best = -1;
    int   best_node = -1;
    float best_score = 0.05f;     /* min threshold */
    for (int i = 0; i < w->pickups.count; ++i) {
        const PickupSpawner *s = &w->pickups.items[i];
        if (s->state != PICKUP_STATE_AVAILABLE) continue;
        if (!mind->pers.uses_powerups && s->kind == PICKUP_POWERUP) continue;
        float need = pickup_need_factor(me, s);
        if (need <= 0.0f) continue;
        float dx = s->pos.x - mp.x;
        float dy = s->pos.y - mp.y;
        float d  = sqrtf(dx*dx + dy*dy);
        float proximity = 1.0f - clampf(d / 1500.0f, 0.0f, 0.85f);
        float score = need * proximity * mind->pers.pickup_priority;
        if (score > best_score) {
            int node = nav_nearest_node(nv, s->pos, 96.0f, BOT_NODE_F_PICKUP);
            /* Fall back to any node when the pickup didn't get folded
             * into the nav (rare — happens if pickup_count > node cap). */
            if (node < 0) node = nav_nearest_node(nv, s->pos, 192.0f, 0);
            if (node < 0) continue;
            best_score = score;
            best       = i;
            best_node  = node;
        }
    }
    *out_node = best_node;
    return best;
}

/* CTF helpers: find this mech's enemy flag + its team's flag carrier. */
static int ctf_enemy_flag_idx(const World *w, int mid) {
    if (w->flag_count != 2) return -1;
    int my_team = w->mechs[mid].team;
    /* world.flags[0] is RED, flags[1] is BLUE by ctf_init_round convention. */
    if (my_team == MATCH_TEAM_RED)  return 1;
    if (my_team == MATCH_TEAM_BLUE) return 0;
    return -1;
}

static int ctf_friendly_flag_idx(const World *w, int mid) {
    int e = ctf_enemy_flag_idx(w, mid);
    if (e < 0) return -1;
    return (e == 0) ? 1 : 0;
}

static float score_engage(const World *w, const BotMind *mind, int mid,
                          MatchModeId mode, int *out_target)
{
    *out_target = -1;
    int enemy = find_nearest_enemy(w, mid, mind->pers.awareness_radius_px, mode);
    if (enemy < 0) return 0.0f;
    *out_target = enemy;
    Vec2 mp = mech_pelvis_pos(w, mid);
    Vec2 ep = mech_pelvis_pos(w, enemy);
    float dx = ep.x - mp.x, dy = ep.y - mp.y;
    float d  = sqrtf(dx*dx + dy*dy);
    float close = 1.0f - clampf(d / 1000.0f, 0.0f, 0.8f);
    float health_factor = health_fraction(&w->mechs[mid]);
    /* Carrier penalty (CTF): we're holding the flag — engage less. */
    if (mode == MATCH_MODE_CTF && ctf_is_carrier(w, mid)) {
        return close * health_factor * mind->pers.aggression * 0.30f;
    }
    return close * (0.4f + 0.6f * health_factor) * mind->pers.aggression;
}

static float score_pursue_pickup(const World *w, const BotMind *mind,
                                 const struct BotNav *nv, int mid,
                                 int *out_node)
{
    *out_node = -1;
    int node = -1;
    int sp   = pick_best_pickup(w, mind, nv, mid, &node);
    if (sp < 0) return 0.0f;
    *out_node = node;
    /* The score is the (need * proximity * priority) value we computed —
     * recompute for the chosen one. */
    const PickupSpawner *s = &w->pickups.items[sp];
    const Mech *me = &w->mechs[mid];
    Vec2 mp = mech_pelvis_pos(w, mid);
    float dx = s->pos.x - mp.x, dy = s->pos.y - mp.y;
    float d  = sqrtf(dx*dx + dy*dy);
    float proximity = 1.0f - clampf(d / 1500.0f, 0.0f, 0.85f);
    float need = pickup_need_factor(me, s);
    return need * proximity * mind->pers.pickup_priority;
}

static float score_grab_flag(const World *w, const BotMind *mind, int mid,
                             MatchModeId mode, int *out_node)
{
    *out_node = -1;
    if (mode != MATCH_MODE_CTF) return 0.0f;
    if (w->flag_count != 2)     return 0.0f;
    if (ctf_is_carrier(w, mid)) return 0.0f;       /* already carrying */

    int ef = ctf_enemy_flag_idx(w, mid);
    if (ef < 0) return 0.0f;
    const Flag *fl = &w->flags[ef];
    if (fl->status == FLAG_CARRIED) return 0.0f;   /* someone has it */

    Vec2 flag_pos = ctf_flag_position(w, ef);
    Vec2 mp = mech_pelvis_pos(w, mid);
    float dx = flag_pos.x - mp.x, dy = flag_pos.y - mp.y;
    float d  = sqrtf(dx*dx + dy*dy);
    float close = 1.0f - clampf(d / 3000.0f, 0.0f, 0.85f);
    float healthy = health_fraction(&w->mechs[mid]);
    *out_node = nav_nearest_node(NULL, flag_pos, 0.0f, 0);  /* filled later */
    return close * healthy * mind->pers.flag_priority;
}

static float score_capture(const World *w, const BotMind *mind, int mid,
                           MatchModeId mode, int *out_node)
{
    *out_node = -1;
    if (mode != MATCH_MODE_CTF) return 0.0f;
    if (!ctf_is_carrier(w, mid)) return 0.0f;

    int ff = ctf_friendly_flag_idx(w, mid);
    if (ff < 0) return 0.0f;
    const Flag *fl = &w->flags[ff];
    /* Capture requires both-flags-home (per 06-ctf.md). If our flag's
     * away, score CAPTURE high anyway — we'll get to base, and the
     * carrier penalty discourages combat. The capture itself will land
     * later once our flag returns. */
    Vec2 home = (Vec2){ fl->home_pos.x, fl->home_pos.y };
    Vec2 mp = mech_pelvis_pos(w, mid);
    float dx = home.x - mp.x, dy = home.y - mp.y;
    float d  = sqrtf(dx*dx + dy*dy);
    float close = 1.0f - clampf(d / 3000.0f, 0.0f, 0.85f);
    float healthy = health_fraction(&w->mechs[mid]);
    return 0.5f + 0.5f * close * healthy * mind->pers.flag_priority;
}

static float score_chase_carrier(const World *w, const BotMind *mind, int mid,
                                 MatchModeId mode, int *out_target)
{
    *out_target = -1;
    if (mode != MATCH_MODE_CTF) return 0.0f;
    int ff = ctf_friendly_flag_idx(w, mid);
    if (ff < 0) return 0.0f;
    const Flag *fl = &w->flags[ff];
    if (fl->status != FLAG_CARRIED) return 0.0f;
    int carrier = fl->carrier_mech;
    if (carrier < 0 || carrier >= w->mech_count) return 0.0f;
    if (!w->mechs[carrier].alive) return 0.0f;
    *out_target = carrier;
    Vec2 mp = mech_pelvis_pos(w, mid);
    Vec2 cp = mech_pelvis_pos(w, carrier);
    float dx = cp.x - mp.x, dy = cp.y - mp.y;
    float d  = sqrtf(dx*dx + dy*dy);
    float close = 1.0f - clampf(d / 2500.0f, 0.0f, 0.85f);
    return 0.4f + 0.5f * close * mind->pers.flag_priority;
}

static float score_return_flag(const World *w, const BotMind *mind, int mid,
                               MatchModeId mode, int *out_node)
{
    *out_node = -1;
    if (mode != MATCH_MODE_CTF) return 0.0f;
    int ff = ctf_friendly_flag_idx(w, mid);
    if (ff < 0) return 0.0f;
    const Flag *fl = &w->flags[ff];
    if (fl->status != FLAG_DROPPED) return 0.0f;
    Vec2 mp = mech_pelvis_pos(w, mid);
    float dx = fl->dropped_pos.x - mp.x, dy = fl->dropped_pos.y - mp.y;
    float d  = sqrtf(dx*dx + dy*dy);
    float close = 1.0f - clampf(d / 2500.0f, 0.0f, 0.85f);
    return 0.45f + 0.45f * close * mind->pers.flag_priority;
}

static float score_retreat(const World *w, const BotMind *mind, int mid)
{
    (void)mind;
    const Mech *me = &w->mechs[mid];
    float hf = health_fraction(me);
    if (hf >= 0.30f) return 0.0f;
    /* Score climbs as HP drops. */
    return (0.30f - hf) / 0.30f * 0.95f;
}

/* Pursue the nearest enemy regardless of awareness / LOS. Always scores
 * something when an enemy is alive; this is the fallback that closes
 * map-wide distance gaps so ENGAGE eventually triggers. Outputs both
 * the target mech (so chase logic knows about it) and a nav node near
 * that enemy (for path planning). */
static int find_any_enemy(const World *w, int mid, MatchModeId mode) {
    const Mech *me = &w->mechs[mid];
    if (!me->alive) return -1;
    Vec2 mp = mech_pelvis_pos(w, mid);
    int best = -1;
    float best_d2 = 1e30f;
    for (int i = 0; i < w->mech_count; ++i) {
        if (i == mid) continue;
        const Mech *o = &w->mechs[i];
        if (!o->alive) continue;
        if (o->is_dummy) continue;
        if (mode != MATCH_MODE_FFA && o->team == me->team) continue;
        Vec2 op = mech_pelvis_pos(w, i);
        float dx = op.x - mp.x, dy = op.y - mp.y;
        float d2 = dx*dx + dy*dy;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return best;
}

static float score_pursue_enemy(const World *w, const BotMind *mind,
                                const struct BotNav *nv, int mid,
                                MatchModeId mode, int *out_node,
                                int *out_target)
{
    *out_node = -1;
    *out_target = -1;
    int e = find_any_enemy(w, mid, mode);
    if (e < 0) return 0.0f;
    *out_target = e;
    Vec2 ep = mech_pelvis_pos(w, e);
    int node = nav_nearest_node(nv, ep, 192.0f, BOT_NODE_F_ON_FLOOR);
    if (node < 0) node = nav_nearest_node(nv, ep, 384.0f, 0);
    *out_node = node;
    /* Baseline 0.40 — beats IDLE (0) and pickups the bot doesn't
     * need, but loses to ENGAGE on close LOS-clear shots and to
     * any high-need pickup (low HP / dry ammo). Bumping above this
     * (0.55+) made small maps stop firing because bots committed
     * to closing distance instead of holding cover-and-shoot. */
    return 0.40f * (0.4f + 0.6f * mind->pers.aggression);
}

static GoalPick run_strategy(const World *w, BotMind *mind, int mid,
                             const struct BotNav *nv, MatchModeId mode)
{
    GoalPick best = { BOT_GOAL_IDLE, -1, -1 };
    float    best_score = 0.0f;

    int eng_target = -1;
    float s_engage = score_engage(w, mind, mid, mode, &eng_target);

    int pickup_node = -1;
    float s_pickup  = score_pursue_pickup(w, mind, nv, mid, &pickup_node);

    int grab_node = -1;
    float s_grab   = score_grab_flag(w, mind, mid, mode, &grab_node);
    if (s_grab > 0.0f && grab_node < 0) {
        /* Re-resolve via nav since score_grab_flag couldn't access nv. */
        int ef = ctf_enemy_flag_idx(w, mid);
        if (ef >= 0) {
            Vec2 fp = ctf_flag_position(w, ef);
            grab_node = nav_nearest_node(nv, fp, 96.0f, BOT_NODE_F_FLAG);
            if (grab_node < 0) grab_node = nav_nearest_node(nv, fp, 192.0f, 0);
        }
    }

    int cap_node = -1;
    float s_cap   = score_capture(w, mind, mid, mode, &cap_node);
    if (s_cap > 0.0f && cap_node < 0) {
        int ff = ctf_friendly_flag_idx(w, mid);
        if (ff >= 0) {
            Vec2 hp = w->flags[ff].home_pos;
            cap_node = nav_nearest_node(nv, hp, 96.0f, BOT_NODE_F_FLAG);
            if (cap_node < 0) cap_node = nav_nearest_node(nv, hp, 192.0f, 0);
        }
    }

    int chase_target = -1;
    float s_chase    = score_chase_carrier(w, mind, mid, mode, &chase_target);

    int return_node = -1;
    float s_return  = score_return_flag(w, mind, mid, mode, &return_node);
    if (s_return > 0.0f) {
        int ff = ctf_friendly_flag_idx(w, mid);
        if (ff >= 0) {
            return_node = nav_nearest_node(nv, w->flags[ff].dropped_pos,
                                           192.0f, 0);
        }
    }

    float s_retreat = score_retreat(w, mind, mid);

    /* Always-on fallback: walk toward the nearest enemy across the
     * map. Scored low enough that PICKUP / ENGAGE / flag goals win
     * when they apply, but high enough that bots eventually meet in
     * the middle even on big maps where awareness < map_width. */
    int pursue_node = -1, pursue_target = -1;
    float s_pursue = score_pursue_enemy(w, mind, nv, mid, mode,
                                        &pursue_node, &pursue_target);

    /* Candidate goals: pick best. */
    struct { BotGoal g; float s; int tm; int tn; } cands[] = {
        { BOT_GOAL_ENGAGE,        s_engage,  eng_target,   -1          },
        { BOT_GOAL_PURSUE_PICKUP, s_pickup,  -1,           pickup_node },
        { BOT_GOAL_GRAB_FLAG,     s_grab,    -1,           grab_node   },
        { BOT_GOAL_CAPTURE,       s_cap,     -1,           cap_node    },
        { BOT_GOAL_CHASE_CARRIER, s_chase,   chase_target, -1          },
        { BOT_GOAL_RETURN_FLAG,   s_return,  -1,           return_node },
        { BOT_GOAL_RETREAT,       s_retreat, -1,           pickup_node },
        { BOT_GOAL_REPOSITION,    s_pursue,  pursue_target,pursue_node },
    };
    int n = (int)(sizeof cands / sizeof cands[0]);
    for (int i = 0; i < n; ++i) {
        if (cands[i].s <= 0.0f) continue;
        if (cands[i].s > best_score) {
            best_score = cands[i].s;
            best.goal  = cands[i].g;
            best.target_mech = cands[i].tm;
            best.target_node = cands[i].tn;
        }
    }
    /* Hysteresis: stick with current goal if it's still scoring and
     * the new winner only marginally beats it. */
    if (mind->goal != BOT_GOAL_IDLE && mind->goal_score_cached > 0.0f) {
        if (best_score < mind->goal_score_cached * BOT_GOAL_HYST_GAIN) {
            best.goal        = (BotGoal)mind->goal;
            best.target_mech = mind->goal_target_mech;
            best.target_node = mind->goal_target_node;
            best_score       = mind->goal_score_cached;
        }
    }
    mind->goal_score_cached = best_score;
    if (best_score <= 0.0f) {
        best.goal = BOT_GOAL_IDLE;
    }
    return best;
}

/* ---- 5. Tactic layer ------------------------------------------------ */

typedef struct {
    Vec2  move_target;
    Vec2  aim_target;
    bool  want_fire;
    bool  want_jet;
    bool  want_jump;
    bool  want_grapple_fire;
    bool  want_grapple_release;
    bool  want_use;
    bool  want_reload;
    bool  want_swap;
    uint8_t next_reach_kind;
} BotWants;

static int bot_current_target_node(const BotMind *mind) {
    if (mind->path_len > 0 && mind->path_step < mind->path_len) {
        return mind->path[mind->path_step];
    }
    return -1;
}

/* Advance path if pelvis is within arrive radius of the head node.
 * Triggers replan when out of nodes. */
static void advance_path(BotMind *mind, const struct BotNav *nv, Vec2 pelv)
{
    if (mind->path_len == 0) return;
    int cur = bot_current_target_node(mind);
    if (cur < 0) return;
    Vec2 np = nv->nodes[cur].pos;
    float dx = np.x - pelv.x, dy = np.y - pelv.y;
    /* Wider arrive radius vertically — bots overshoot on JET hops. */
    float arrive = 36.0f;
    if (fabsf(dx) < arrive && fabsf(dy) < arrive * 1.5f) {
        mind->path_step++;
        if (mind->path_step >= mind->path_len) {
            mind->path_len = 0;
        } else {
            /* Look up the reach we just consumed to set next_reach_kind.
             * Cheap O(reach_count_of_prev) scan. */
            int prev = (mind->path_step >= 1) ? mind->path[mind->path_step - 1]
                                              : -1;
            int next = mind->path[mind->path_step];
            if (prev >= 0) {
                const BotNavNode *pn = &nv->nodes[prev];
                for (int k = 0; k < pn->reach_count; ++k) {
                    const BotNavReach *r = &nv->reaches[pn->reach_first + k];
                    if (r->to_node == next) {
                        mind->path_reach_kind = r->kind;
                        break;
                    }
                }
            }
        }
    }
}

/* Pick a varied reposition target — a node that ISN'T too close to
 * the bot's current position so heatmap coverage actually spreads.
 * Without the min-distance filter the random walk biased toward
 * "stay near spawn" because half the nodes were within a tile. */
static int pick_reposition_node(const struct BotNav *nv, Vec2 from,
                                pcg32_t *rng)
{
    if (!nv || nv->node_count == 0) return -1;
    /* Prefer distant SPAWN or ON_FLOOR nodes (≥ 1200 px away).
     * Successive tries widen the band so big maps still find a
     * target. */
    int tries = 8;
    while (tries-- > 0) {
        uint32_t r = pcg32_next(rng) % (uint32_t)nv->node_count;
        const BotNavNode *n = &nv->nodes[r];
        if (!(n->flags & (BOT_NODE_F_SPAWN | BOT_NODE_F_ON_FLOOR))) continue;
        float dx = n->pos.x - from.x, dy = n->pos.y - from.y;
        float d2 = dx*dx + dy*dy;
        if (d2 < 1200.0f * 1200.0f) continue;     /* too close */
        return (int)r;
    }
    /* Fallback: any node, even close. */
    return (int)(pcg32_next(rng) % (uint32_t)nv->node_count);
}

static void tactic_path_follow(BotMind *mind, const struct BotNav *nv,
                               const World *w, int mid, BotWants *out)
{
    Vec2 pelv = mech_pelvis_pos(w, mid);
    advance_path(mind, nv, pelv);
    int cur = bot_current_target_node(mind);
    if (cur < 0) {
        /* No path — wander toward current target_node if any. */
        if (mind->goal_target_node >= 0 &&
            mind->goal_target_node < nv->node_count)
        {
            out->move_target = nv->nodes[mind->goal_target_node].pos;
        } else {
            out->move_target = pelv;
        }
        return;
    }
    Vec2 np = nv->nodes[cur].pos;
    out->move_target = np;

    /* If the next hop is a JET reach, request jet thrust whenever the
     * target is above. */
    if (mind->path_reach_kind == BOT_REACH_JET && np.y < pelv.y - 12.0f) {
        out->want_jet = true;
    }
    /* JUMP reach: leading-edge jump request when pelvis is approaching
     * the take-off X. */
    if (mind->path_reach_kind == BOT_REACH_JUMP && np.y < pelv.y - 8.0f) {
        if (w->mechs[mid].grounded &&
            w->tick - mind->last_jump_tick > 12)
        {
            out->want_jump = true;
        }
    }
    /* GRAPPLE reach: fire on first tick of the hop. */
    if (mind->path_reach_kind == BOT_REACH_GRAPPLE &&
        mind->pers.grapple_priority > 0.30f &&
        w->mechs[mid].secondary_id == WEAPON_GRAPPLING_HOOK &&
        w->mechs[mid].grapple.state == GRAPPLE_IDLE)
    {
        out->aim_target = np;
        out->want_grapple_fire = true;
    }
}

static void tactic_engage(BotMind *mind, const struct BotNav *nv,
                          const World *w, int mid, int enemy_id, BotWants *out)
{
    Vec2 mp = mech_pelvis_pos(w, mid);
    Vec2 ep = mech_pelvis_pos(w, enemy_id);
    /* Aim at chest, not pelvis. */
    Vec2 chest = (Vec2){ ep.x, ep.y - 24.0f };
    /* Optional lead. */
    if (mind->pers.aim_lead_frac > 0.0f) {
        Vec2 ev = mech_pelvis_vel(w, enemy_id);
        /* Projectile time-of-flight ~= dist / 800 px/s (rough). */
        float dx = chest.x - mp.x, dy = chest.y - mp.y;
        float d  = sqrtf(dx*dx + dy*dy);
        float tof = d / 800.0f;
        chest.x += ev.x * tof * mind->pers.aim_lead_frac;
        chest.y += ev.y * tof * mind->pers.aim_lead_frac;
    }
    out->aim_target = chest;

    /* LOS check — only fire if we can see them. */
    bool los = los_clear(w, mp, ep);

    /* Health-low retreat: don't fire forward when we're rotating away. */
    float hf = health_fraction(&w->mechs[mid]);

    if (los && hf > 0.20f) {
        out->want_fire = true;
    } else if (!los) {
        /* No LOS — close distance by walking toward the enemy. */
        out->move_target = ep;
        /* If they're above, lean on jet. */
        if (ep.y < mp.y - 64.0f) out->want_jet = true;
        return;
    }

    /* Strafe / advance: a small lateral offset based on tick parity. */
    float side = ((w->tick / 30) & 1u) ? 1.0f : -1.0f;
    float ideal_dist = 400.0f;
    float dx = ep.x - mp.x;
    float ddx = (fabsf(dx) > ideal_dist + 60.0f)
                ? (dx > 0 ? 80.0f : -80.0f)
                : (fabsf(dx) < ideal_dist - 60.0f)
                  ? (dx > 0 ? -80.0f : 80.0f)
                  : 0.0f;
    out->move_target = (Vec2){ mp.x + ddx + side * 40.0f, mp.y };

    /* Engineer chassis: BTN_USE drops a repair pack when hurt. */
    if (w->mechs[mid].chassis_id == CHASSIS_ENGINEER && hf < 0.60f &&
        w->mechs[mid].ability_cooldown <= 0.0f)
    {
        out->want_use = true;
    }

    /* If ammo is dry, reload. */
    if (w->mechs[mid].ammo == 0 && w->mechs[mid].ammo_max > 0) {
        out->want_reload = true;
        out->want_fire   = false;
    }

    /* Auto-pull grapple if we're hooked + holding ENGAGE. */
    if (w->mechs[mid].grapple.state == GRAPPLE_ATTACHED) {
        /* Release once we're close to the anchor. */
        Vec2 a = w->mechs[mid].grapple.anchor_pos;
        float gdx = a.x - mp.x, gdy = a.y - mp.y;
        if (gdx*gdx + gdy*gdy < 96.0f * 96.0f) {
            out->want_grapple_release = true;
        }
    }

    (void)nv;
}

static void tactic_pursue_node(BotMind *mind, const struct BotNav *nv,
                               const World *w, int mid, BotWants *out)
{
    Vec2 mp = mech_pelvis_pos(w, mid);
    /* Aim along travel direction so the mech walks "forward". */
    int cur = bot_current_target_node(mind);
    if (cur >= 0) {
        Vec2 np = nv->nodes[cur].pos;
        out->aim_target = np;
    } else if (mind->goal_target_node >= 0 &&
               mind->goal_target_node < nv->node_count) {
        out->aim_target = nv->nodes[mind->goal_target_node].pos;
    } else {
        out->aim_target = mp;
    }
    tactic_path_follow(mind, nv, w, mid, out);
}

static void tactic_retreat(BotMind *mind, const struct BotNav *nv,
                           const World *w, int mid, BotWants *out)
{
    tactic_pursue_node(mind, nv, w, mid, out);
}

/* ---- 6. Motor ------------------------------------------------------- */

/* Slew `cur` toward `target` by at most `max_rad` (radians). */
static Vec2 slew_aim(Vec2 cur_origin, Vec2 cur, Vec2 target, float max_rad) {
    Vec2 d_cur = (Vec2){ cur.x - cur_origin.x, cur.y - cur_origin.y };
    Vec2 d_tgt = (Vec2){ target.x - cur_origin.x, target.y - cur_origin.y };
    float a_cur = atan2f(d_cur.y, d_cur.x);
    float a_tgt = atan2f(d_tgt.y, d_tgt.x);
    float da = a_tgt - a_cur;
    while (da >  PI) da -= 2.0f * PI;
    while (da < -PI) da += 2.0f * PI;
    if (da >  max_rad) da =  max_rad;
    if (da < -max_rad) da = -max_rad;
    float r = sqrtf(d_tgt.x * d_tgt.x + d_tgt.y * d_tgt.y);
    if (r < 1.0f) r = 1.0f;
    float a_new = a_cur + da;
    return (Vec2){ cur_origin.x + cosf(a_new) * r,
                   cur_origin.y + sinf(a_new) * r };
}

static ClientInput run_motor(World *w, int mid, BotMind *mind,
                             const BotWants *wants)
{
    ClientInput in = {0};
    in.dt  = BOT_TICK_DT;
    in.seq = (uint16_t)(w->tick & 0xFFFFu);
    Mech *m = &w->mechs[mid];
    Vec2 pelv = mech_pelvis_pos(w, mid);

    /* Aim slew. */
    if (mind->cur_aim.x == 0.0f && mind->cur_aim.y == 0.0f) {
        mind->cur_aim = wants->aim_target;
    } else {
        mind->cur_aim = slew_aim(pelv, mind->cur_aim, wants->aim_target,
                                  mind->pers.aim_slew_rad_per_tick);
    }

    /* Jitter on fire ticks. */
    Vec2 aim = mind->cur_aim;
    if (wants->want_fire && mind->pers.aim_jitter_rad > 0.0f) {
        float a = pcg32_float01(&mind->rng) * 2.0f * PI;
        float r = pcg32_float01(&mind->rng) *
                  mind->pers.aim_jitter_rad * 600.0f;
        aim.x += cosf(a) * r;
        aim.y += sinf(a) * r;
    }
    in.aim_x = aim.x;
    in.aim_y = aim.y;
    m->aim_world = (Vec2){ in.aim_x, in.aim_y };

    /* Horizontal movement intent. */
    float dx = wants->move_target.x - pelv.x;
    if (fabsf(dx) > 12.0f) {
        in.buttons |= (dx > 0) ? BTN_RIGHT : BTN_LEFT;
    }

    /* Vertical: explicit jet (path tells us when), gated on fuel.
     * Hysteresis: once fuel falls below 10 % we lock out JET until
     * the gauge refills to 40 %. Without this gate the bot would
     * mash JET against an empty tank — the input flows but the
     * chassis applies no thrust (apply_jet_force gates on fuel
     * internally), so the bot looks frozen with JET held. With the
     * lock-out, the bot releases JET, lets the gauge regen while
     * grounded (chassis.fuel_regen), and re-engages at 40 %. The
     * 40 % retry floor keeps "press, fuel hits zero, release,
     * regen one tick, press again" chatter out of the wire. */
    float fuel_frac = (m->fuel_max > 0.0f) ? (m->fuel / m->fuel_max) : 0.0f;
    if (mind->jet_locked_out) {
        if (fuel_frac >= 0.40f) mind->jet_locked_out = false;
    } else if (fuel_frac < 0.10f) {
        mind->jet_locked_out = true;
    }
    bool jet_ok = !mind->jet_locked_out && fuel_frac > 0.10f;
    if (wants->want_jet && jet_ok) in.buttons |= BTN_JET;
    if (wants->want_jump) {
        in.buttons |= BTN_JUMP;
        mind->last_jump_tick = w->tick;
    }

    /* Fire — adaptive cadence. For charge or spin-up weapons (Rail
     * Cannon, Microgun) we hold BTN_FIRE continuously so the charge
     * actually builds. For everything else we pulse with a duty cycle
     * so edge-triggered single-shots (Sidearm, Frag Grenade trigger)
     * re-edge cleanly and auto weapons don't notice. */
    if (wants->want_fire) {
        const Weapon *wpn = weapon_def(m->weapon_id);
        bool hold = (wpn && wpn->charge_sec > 0.0f);
        if (hold || (w->tick % 10) < 5) {
            in.buttons |= BTN_FIRE;
        }
    }

    if (wants->want_grapple_fire)    in.buttons |= BTN_FIRE_SECONDARY;
    if (wants->want_grapple_release) in.buttons |= BTN_USE;
    if (wants->want_use && !wants->want_grapple_release) in.buttons |= BTN_USE;
    if (wants->want_reload) in.buttons |= BTN_RELOAD;
    if (wants->want_swap)   in.buttons |= BTN_SWAP;

    /* Auto-recover from being stuck: jet (if fuel) + jump, and on
     * persistent stuck flip the walk direction so we don't keep
     * pushing the same wall. Without the fuel gate, a bot stuck in
     * a corner would mash JET against an empty tank and never
     * recover; the wall-flip is what actually un-jams them. */
    if (fabsf(pelv.x - mind->stuck_last_x) < 0.5f) {
        if (mind->stuck_since_tick == 0) mind->stuck_since_tick = w->tick;
        uint64_t stuck_for = w->tick - mind->stuck_since_tick;
        if (stuck_for > BOT_STUCK_TICKS) {
            if (jet_ok && (w->tick % 4) < 3) in.buttons |= BTN_JET;
            if ((w->tick % 30) == 0) in.buttons |= BTN_JUMP;
        }
        /* Still stuck after ~4 s — flip the walking direction so
         * the bot stops slamming into whatever's in the way. */
        if (stuck_for > BOT_STUCK_TICKS * 4) {
            in.buttons &= (uint16_t)~(BTN_LEFT | BTN_RIGHT);
            in.buttons |= (dx > 0) ? BTN_LEFT : BTN_RIGHT;
        }
    } else {
        mind->stuck_since_tick = 0;
    }
    mind->stuck_last_x = pelv.x;

    return in;
}

/* ---- Reaction delay ------------------------------------------------- */

static void update_reaction(BotMind *mind, const World *w, int prev_target,
                            int new_target)
{
    if (new_target != prev_target) {
        if (new_target >= 0) {
            mind->first_seen_enemy_tick = w->tick;
            mind->reaction_ticks_remaining =
                (int16_t)(mind->pers.reaction_ticks + 0.5f);
        } else {
            mind->reaction_ticks_remaining = 0;
        }
    }
    if (mind->reaction_ticks_remaining > 0) {
        mind->reaction_ticks_remaining--;
    }
}

/* ---- 7. Public API -------------------------------------------------- */

void bot_system_init(BotSystem *bs) {
    if (!bs) return;
    memset(bs, 0, sizeof *bs);
    for (int i = 0; i < MAX_MECHS; ++i) {
        bs->minds[i].mech_id = -1;
        bs->minds[i].goal_target_mech = -1;
        bs->minds[i].goal_target_node = -1;
        bs->minds[i].seen_enemy_id    = -1;
    }
    bs->seed = 0xC0FFEE5EEDuLL;
}

void bot_system_reset_minds(BotSystem *bs) {
    if (!bs) return;
    for (int i = 0; i < MAX_MECHS; ++i) {
        bs->minds[i].in_use = false;
        bs->minds[i].mech_id = -1;
        bs->minds[i].path_len = 0;
        bs->minds[i].path_step = 0;
        bs->minds[i].goal = BOT_GOAL_IDLE;
        bs->minds[i].goal_target_mech = -1;
        bs->minds[i].goal_target_node = -1;
        bs->minds[i].goal_score_cached = 0.0f;
        bs->minds[i].seen_enemy_id = -1;
        bs->minds[i].cur_aim = (Vec2){ 0.0f, 0.0f };
        bs->minds[i].last_strategy_tick = 0;
        bs->minds[i].last_replan_tick = 0;
        bs->minds[i].first_seen_enemy_tick = 0;
        bs->minds[i].last_jump_tick = 0;
        bs->minds[i].stuck_since_tick = 0;
        bs->minds[i].stuck_last_x = 0.0f;
        bs->minds[i].reaction_ticks_remaining = 0;
    }
    bs->count = 0;
    bs->nav   = NULL;     /* nav is level-arena owned; caller reset that arena */
}

void bot_system_destroy(BotSystem *bs) {
    if (!bs) return;
    bs->nav = NULL;
    bs->count = 0;
}

int bot_system_build_nav(BotSystem *bs, const Level *level, Arena *arena) {
    if (!bs || !level || !arena) return 0;
    if (level->width <= 0 || level->height <= 0) return 0;

    struct BotNav *nv = ARENA_NEW(arena, struct BotNav);
    if (!nv) {
        LOG_W("bot_nav: arena out of space; bots will run nav-less");
        bs->nav = NULL;
        return 0;
    }
    memset(nv, 0, sizeof *nv);
    nv->level_w_px = (float)(level->width  * level->tile_size);
    nv->level_h_px = (float)(level->height * level->tile_size);
    nv->tile_size  = level->tile_size;

    build_floor_nodes  (nv, level);
    build_spawn_nodes  (nv, level);
    build_pickup_nodes (nv, level);
    build_flag_nodes   (nv, level);

    /* Snap spawn / pickup / flag nodes onto the nearest floor sample by
     * lowering them to the surface. (Designers place pickups one tile
     * above floor; the nav build will pull them down so paths land at
     * the same row as the WALK strip.) */
    for (int i = 0; i < nv->node_count; ++i) {
        BotNavNode *n = &nv->nodes[i];
        if (n->flags & BOT_NODE_F_ON_FLOOR) continue;
        int ts = level->tile_size;
        int tx = (int)(n->pos.x / (float)ts);
        int ty = (int)(n->pos.y / (float)ts);
        for (int d = 0; d < 8; ++d) {
            if (tile_is_floor_top(level, tx, ty + d)) {
                n->pos.y = (ty + d) * (float)ts - 2.0f;
                break;
            }
        }
    }

    /* Rebuild the spatial hash. */
    nav_grid_clear();
    for (int i = 0; i < nv->node_count; ++i) nav_grid_add(nv, i);

    build_reachabilities(nv, level);

    bs->nav = nv;
    LOG_I("bot_nav: built %d nodes, %d reachabilities on %dx%d map",
          nv->node_count, nv->reach_count, level->width, level->height);
    return nv->node_count;
}

void bot_attach(BotSystem *bs, int mech_id, BotTier tier, uint64_t seed_salt) {
    if (!bs) return;
    if (mech_id < 0 || mech_id >= MAX_MECHS) return;
    BotMind *m = &bs->minds[mech_id];
    memset(m, 0, sizeof *m);
    m->mech_id            = (int8_t)mech_id;
    m->in_use             = true;
    m->pers               = bot_personality_for_tier(tier);
    m->goal               = BOT_GOAL_IDLE;
    m->goal_target_mech   = -1;
    m->goal_target_node   = -1;
    m->seen_enemy_id      = -1;
    pcg32_seed(&m->rng,
               (bs->seed ^ ((uint64_t)mech_id << 16) ^ seed_salt),
               (uint64_t)(mech_id + 1) * 2654435761uLL);
    bs->count++;
}

void bot_detach(BotSystem *bs, int mech_id) {
    if (!bs) return;
    if (mech_id < 0 || mech_id >= MAX_MECHS) return;
    BotMind *m = &bs->minds[mech_id];
    if (!m->in_use) return;
    m->in_use = false;
    m->mech_id = -1;
    if (bs->count > 0) bs->count--;
}

bool bot_is_attached(const BotSystem *bs, int mech_id) {
    if (!bs) return false;
    if (mech_id < 0 || mech_id >= MAX_MECHS) return false;
    return bs->minds[mech_id].in_use;
}

int bot_nav_node_count(const BotSystem *bs) {
    if (!bs || !bs->nav) return 0;
    return bs->nav->node_count;
}

bool bot_nav_node_pos(const BotSystem *bs, int node_id, Vec2 *out) {
    if (!bs || !bs->nav || !out) return false;
    if (node_id < 0 || node_id >= bs->nav->node_count) return false;
    *out = bs->nav->nodes[node_id].pos;
    return true;
}

/* The big per-tick step. */
void bot_step(BotSystem *bs, World *w, struct Game *g, float dt) {
    (void)dt;
    if (!bs || !w) return;

    MatchModeId mode = MATCH_MODE_FFA;
    if (g) {
        mode = (MatchModeId)g->match.mode;
    } else {
        mode = (MatchModeId)w->match_mode_cached;
    }

    for (int i = 0; i < MAX_MECHS; ++i) {
        BotMind *mind = &bs->minds[i];
        if (!mind->in_use) continue;
        int mid = mind->mech_id;
        if (mid < 0 || mid >= w->mech_count) continue;
        Mech *m = &w->mechs[mid];
        if (!m->alive) {
            /* Dead bot: hold idle input. */
            m->latched_input = (ClientInput){ .dt = BOT_TICK_DT };
            continue;
        }

        /* Strategy step at 10 Hz, staggered by mech_id so multiple
         * bots' strategy passes don't all land on the same tick. */
        bool strategy_now =
            ((w->tick + (uint64_t)mid) % BOT_STRATEGY_INTERVAL == 0) ||
            (mind->last_strategy_tick == 0);
        if (strategy_now) {
            int prev_target = mind->goal_target_mech;
            GoalPick pick = run_strategy(w, mind, mid, bs->nav, mode);
            mind->goal             = (uint8_t)pick.goal;
            mind->goal_target_mech = (int16_t)pick.target_mech;
            mind->goal_target_node = (int16_t)pick.target_node;
            mind->last_strategy_tick = w->tick;
            update_reaction(mind, w, prev_target, pick.target_mech);

            /* Plan path when the goal involves a target node. */
            if (pick.target_node >= 0 && bs->nav && bs->nav->node_count > 0) {
                Vec2 pelv = mech_pelvis_pos(w, mid);
                int start = nav_nearest_node(bs->nav, pelv, 256.0f,
                                              BOT_NODE_F_ON_FLOOR);
                if (start < 0) start = nav_nearest_node(bs->nav, pelv, 512.0f, 0);
                if (start >= 0) {
                    if (start == pick.target_node) {
                        /* Already there. */
                        mind->path_len = 1;
                        mind->path[0]  = (int16_t)pick.target_node;
                        mind->path_step = 0;
                        mind->path_reach_kind = BOT_REACH_WALK;
                    } else {
                        if (!bot_plan_path(mind, bs->nav, start, pick.target_node)) {
                            /* No path — wander toward target_node directly. */
                            mind->path_len = 1;
                            mind->path[0]  = (int16_t)pick.target_node;
                            mind->path_step = 0;
                            mind->path_reach_kind = BOT_REACH_WALK;
                        }
                    }
                    mind->last_replan_tick = w->tick;
                }
            }
            /* REPOSITION fallback when goal is IDLE — pick a random
             * spawn node so heatmap coverage stays alive in the bake. */
            if (mind->goal == BOT_GOAL_IDLE && bs->nav && bs->nav->node_count > 0) {
                Vec2 pelv = mech_pelvis_pos(w, mid);
                int node  = pick_reposition_node(bs->nav, pelv, &mind->rng);
                if (node >= 0) {
                    mind->goal             = BOT_GOAL_REPOSITION;
                    mind->goal_target_node = (int16_t)node;
                    int start = nav_nearest_node(bs->nav, pelv, 256.0f,
                                                  BOT_NODE_F_ON_FLOOR);
                    if (start < 0) start = nav_nearest_node(bs->nav, pelv, 512.0f, 0);
                    if (start >= 0 && start != node) {
                        bot_plan_path(mind, bs->nav, start, node);
                    } else {
                        mind->path_len = 1;
                        mind->path[0]  = (int16_t)node;
                        mind->path_step = 0;
                        mind->path_reach_kind = BOT_REACH_WALK;
                    }
                }
            }
        }

        /* Tactic — dispatch on goal. */
        BotWants wants = (BotWants){0};
        wants.move_target = mech_pelvis_pos(w, mid);
        wants.aim_target  = wants.move_target;

        bool engage_locked = (mind->goal == BOT_GOAL_ENGAGE) &&
                             (mind->goal_target_mech >= 0) &&
                             (mind->goal_target_mech < w->mech_count) &&
                             (w->mechs[mind->goal_target_mech].alive);
        if (engage_locked && mind->reaction_ticks_remaining > 0) {
            /* Still reacting — don't snap aim yet. Walk vaguely toward
             * the enemy meanwhile so we close range. */
            wants.move_target = mech_pelvis_pos(w, mind->goal_target_mech);
        } else if (engage_locked) {
            tactic_engage(mind, bs->nav, w, mid, mind->goal_target_mech, &wants);
        } else if (mind->goal == BOT_GOAL_CHASE_CARRIER &&
                   mind->goal_target_mech >= 0 &&
                   mind->goal_target_mech < w->mech_count &&
                   w->mechs[mind->goal_target_mech].alive)
        {
            /* Chase = treat like engage on the carrier. */
            tactic_engage(mind, bs->nav, w, mid, mind->goal_target_mech, &wants);
        } else if (mind->goal == BOT_GOAL_RETREAT) {
            tactic_retreat(mind, bs->nav, w, mid, &wants);
        } else if (mind->goal_target_node >= 0 && bs->nav &&
                   mind->goal_target_node < bs->nav->node_count)
        {
            tactic_pursue_node(mind, bs->nav, w, mid, &wants);
        } else {
            /* IDLE — face aim forward; no movement. */
            Vec2 pelv = mech_pelvis_pos(w, mid);
            wants.aim_target = (Vec2){ pelv.x + (m->facing_left ? -100.0f : 100.0f),
                                        pelv.y };
        }

        /* Opportunistic fire — if there's *any* LOS-clear enemy
         * within ~1.6× awareness, fire on it regardless of which
         * goal won this strategy pass. Without this, bots on big
         * maps (Foundry, Concourse, Citadel) never engage because
         * the pickup/pursue scorers keep the goal away from ENGAGE
         * even when an enemy is in plain sight. */
        if (!wants.want_fire) {
            float scan_r = mind->pers.awareness_radius_px * 1.6f;
            if (scan_r < 1200.0f) scan_r = 1200.0f;
            int eo = find_nearest_enemy(w, mid, scan_r, mode);
            if (eo >= 0) {
                Vec2 mp = mech_pelvis_pos(w, mid);
                Vec2 ep = mech_pelvis_pos(w, eo);
                /* Aim at chest. Apply a tiny lead so movement-vs-static
                 * shots still hit. */
                Vec2 chest = (Vec2){ ep.x, ep.y - 24.0f };
                if (mind->pers.aim_lead_frac > 0.0f) {
                    Vec2 ev = mech_pelvis_vel(w, eo);
                    float dx = chest.x - mp.x, dy = chest.y - mp.y;
                    float d  = sqrtf(dx*dx + dy*dy);
                    float tof = d / 800.0f;
                    chest.x += ev.x * tof * mind->pers.aim_lead_frac;
                    chest.y += ev.y * tof * mind->pers.aim_lead_frac;
                }
                wants.aim_target = chest;
                wants.want_fire  = true;
            }
        }

        /* Motor. */
        ClientInput in = run_motor(w, mid, mind, &wants);
        m->latched_input = in;
    }
}
