#include "physics.h"

#include "level.h"
#include "log.h"

#include <math.h>
#include <string.h>

/* ---- Gravity ------------------------------------------------------- */

/* Forward decl — implemented further down (Task 8b). */
static void apply_ambient_zone_overrides(World *w, float dt,
                                         uint8_t *zero_g_mask);

void physics_apply_gravity(World *w, float dt) {
    ParticlePool *p = &w->particles;
    Vec2 g = w->level.gravity;

    /* P02: zero per-particle contact data at the start of each tick.
     * Whatever fresh contacts the relaxation pass produces overwrite
     * these; particles with no contact this tick read as (0, 0, 0). */
    if (p->contact_nx_q) memset(p->contact_nx_q, 0, (size_t)p->count);
    if (p->contact_ny_q) memset(p->contact_ny_q, 0, (size_t)p->count);
    if (p->contact_kind) memset(p->contact_kind, 0, (size_t)p->count);

    /* ZERO_G ambient zones: build a per-particle bitmask of "skip
     * gravity" so the gravity loop can branch cheaply. The mask lives
     * on the frame stack; capacity matches the pool. */
    uint8_t zero_g_local[PARTICLES_CAPACITY];
    uint8_t *zero_g = NULL;
    if (w->level.ambi_count > 0) {
        memset(zero_g_local, 0, (size_t)p->count);
        zero_g = zero_g_local;
    }

    /* Verlet absorbs accelerations as `+ a*dt^2` inside the integrate
     * step. We apply a velocity nudge here by displacing pos directly,
     * which accumulates into the (pos - prev) term on the next integrate.
     *
     * That's equivalent and lets us layer per-frame forces (jet, run,
     * recoil) the same way without double-bookkeeping a velocity field. */
    apply_ambient_zone_overrides(w, dt, zero_g);

    float fx = g.x * dt * dt;
    float fy = g.y * dt * dt;
    for (int i = 0; i < p->count; ++i) {
        if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
        if (p->inv_mass[i] <= 0.0f) continue;
        if (zero_g && zero_g[i]) continue;
        p->pos_x[i] += fx;
        p->pos_y[i] += fy;
    }
}

/* ---- Verlet integrate --------------------------------------------- */

void physics_integrate(World *w, float dt) {
    (void)dt;   /* dt enters via the (pos - prev) implicit velocity */
    ParticlePool *p = &w->particles;
    const float damp = PHYSICS_VELOCITY_DAMP;

    /* Swept collision against tiles. The discrete inside-tile escape
     * in collide_map_one_pass only fires if the post-integrate center
     * lands inside a solid tile. With sustained jet from below, a
     * particle can build up enough velocity to skip clear over a
     * 1-tile-thick platform in one tick (it's below the platform
     * before integrate, above the platform after — never inside).
     * Ray-cast from prev to the integrated pos and clamp the move at
     * the first solid we hit so the post-integrate pos always lies
     * just shy of the tile we'd otherwise have tunneled through. */
    const Level *L = &w->level;
    const float r  = PHYSICS_PARTICLE_RADIUS;

    for (int i = 0; i < p->count; ++i) {
        if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
        if (p->inv_mass[i] <= 0.0f) continue;

        float px = p->pos_x[i],  py = p->pos_y[i];
        float qx = p->prev_x[i], qy = p->prev_y[i];

        /* x_{n+1} = x_n + (x_n - x_{n-1}) * damp  (forces already added) */
        float nx = px + (px - qx) * damp;
        float ny = py + (py - qy) * damp;

        /* Sweep from start-of-tick prev to the integrated pos. We
         * sweep from prev (not from px, the post-force pre-integrate
         * pos) because forces alone can also push a particle through
         * a tile in one tick. If the path hits a solid before the
         * end, pull back the move to land r pixels short of the
         * crossing on the side we came from. */
        float dx = nx - qx;
        float dy = ny - qy;
        float seg2 = dx * dx + dy * dy;
        if (seg2 > 1.0f) {
            float t = 1.0f;
            if (level_ray_hits(L, (Vec2){qx, qy}, (Vec2){nx, ny}, &t)) {
                float seg_len = sqrtf(seg2);
                /* Stop r+epsilon px before the tile boundary. */
                float t_clamped = t - (r + 0.5f) / seg_len;
                if (t_clamped < 0.0f) t_clamped = 0.0f;
                nx = qx + dx * t_clamped;
                ny = qy + dy * t_clamped;
            }
        }

        p->prev_x[i] = px;
        p->prev_y[i] = py;
        p->pos_x[i] = nx;
        p->pos_y[i] = ny;
    }
    /* GROUNDED bits are cleared inside physics_constrain_and_collide. */
}

/* ---- Constraint relaxation ---------------------------------------- */

static void solve_distance(ParticlePool *p, const Constraint *c) {
    float ax = p->pos_x[c->a], ay = p->pos_y[c->a];
    float bx = p->pos_x[c->b], by = p->pos_y[c->b];
    float dx = bx - ax, dy = by - ay;
    float d2 = dx * dx + dy * dy;
    if (d2 < 1e-6f) return;
    float d = sqrtf(d2);
    float diff = (d - c->rest) / d;

    float wa = p->inv_mass[c->a];
    float wb = p->inv_mass[c->b];
    float wsum = wa + wb;
    if (wsum < 1e-6f) return;

    float ka = (wa / wsum) * 0.5f;
    float kb = (wb / wsum) * 0.5f;

    p->pos_x[c->a] += dx * diff * ka;
    p->pos_y[c->a] += dy * diff * ka;
    p->pos_x[c->b] -= dx * diff * kb;
    p->pos_y[c->b] -= dy * diff * kb;
}

static void solve_distance_limit(ParticlePool *p, const Constraint *c) {
    float ax = p->pos_x[c->a], ay = p->pos_y[c->a];
    float bx = p->pos_x[c->b], by = p->pos_y[c->b];
    float dx = bx - ax, dy = by - ay;
    float d2 = dx * dx + dy * dy;
    if (d2 < 1e-6f) return;
    float d = sqrtf(d2);
    float target = d;
    if (d < c->min_len) target = c->min_len;
    else if (d > c->max_len) target = c->max_len;
    else return;

    float diff = (d - target) / d;

    float wa = p->inv_mass[c->a];
    float wb = p->inv_mass[c->b];
    float wsum = wa + wb;
    if (wsum < 1e-6f) return;

    float ka = (wa / wsum) * 0.5f;
    float kb = (wb / wsum) * 0.5f;

    p->pos_x[c->a] += dx * diff * ka;
    p->pos_y[c->a] += dy * diff * ka;
    p->pos_x[c->b] -= dx * diff * kb;
    p->pos_y[c->b] -= dy * diff * kb;
}

/* Angle constraint: clamp the interior angle at joint B (between B→A
 * and B→C) into [min_ang, max_ang]. The previous implementation used
 * atan2-difference + modulo, which wraps catastrophically near the
 * π boundary (a "straight chain" can flip to -π and clamp to a giant
 * correction). Using acos(dot) gives a stable angle in [0, π]. */
static void solve_angle(ParticlePool *p, const Constraint *c) {
    int A = c->a, B = c->c, C = c->b;
    float bx = p->pos_x[B], by = p->pos_y[B];
    float ax = p->pos_x[A] - bx, ay = p->pos_y[A] - by;
    float cx = p->pos_x[C] - bx, cy = p->pos_y[C] - by;

    float la = sqrtf(ax * ax + ay * ay);
    float lc = sqrtf(cx * cx + cy * cy);
    if (la < 1e-6f || lc < 1e-6f) return;

    float dot = (ax * cx + ay * cy) / (la * lc);
    if (dot >  1.0f) dot =  1.0f;
    if (dot < -1.0f) dot = -1.0f;
    float angle = acosf(dot);          /* [0, π]; π = straight chain */

    float clamped = angle;
    if (clamped < c->min_ang)  clamped = c->min_ang;
    else if (clamped > c->max_ang) clamped = c->max_ang;
    if (fabsf(clamped - angle) < 1e-4f) return;

    /* Rotation sign: cross > 0 → C is CCW from A. Opening the angle
     * (clamped > angle) means rotating A clockwise and C counter-
     * clockwise (when cross > 0); flip when cross < 0. We apply half
     * the correction to each endpoint. */
    float cross = ax * cy - ay * cx;
    float sign  = (cross >= 0.0f) ? 1.0f : -1.0f;
    float corr  = (clamped - angle) * 0.5f * sign;

    float ca = cosf(-corr), sa = sinf(-corr);
    float cc = cosf( corr), sc = sinf( corr);

    float new_ax = ax * ca - ay * sa;
    float new_ay = ax * sa + ay * ca;
    float new_cx = cx * cc - cy * sc;
    float new_cy = cx * sc + cy * cc;

    if (p->inv_mass[A] > 0.0f) {
        p->pos_x[A] = bx + new_ax;
        p->pos_y[A] = by + new_ay;
    }
    if (p->inv_mass[C] > 0.0f) {
        p->pos_x[C] = bx + new_cx;
        p->pos_y[C] = by + new_cy;
    }
}

/* CSTR_FIXED_ANCHOR (P06): one-sided distance limit toward the
 * constraint's inline `fixed_pos`. When the particle is farther than
 * `rest`, pull it in. When closer, the rope is slack — no force.
 * End b has effective inv_mass = 0 (it's a fixed world point), so the
 * particle takes the full correction.
 *
 * Used by the grappling hook when anchored to a tile. The one-sided
 * behaviour gives the firer the "Tarzan swing" feel: the body hangs
 * at rope length, swings as a pendulum, and can drift closer to the
 * anchor (slack) without being shoved away. */
static void solve_fixed_anchor(ParticlePool *p, const Constraint *c) {
    int ai = c->a;
    if (p->inv_mass[ai] <= 0.0f) return;
    float dx = c->fixed_pos.x - p->pos_x[ai];
    float dy = c->fixed_pos.y - p->pos_y[ai];
    float d2 = dx * dx + dy * dy;
    if (d2 < 1e-6f) return;
    float d = sqrtf(d2);
    if (d <= c->rest) return;            /* slack — no force */
    float diff = (d - c->rest) / d;
    p->pos_x[ai] += dx * diff;
    p->pos_y[ai] += dy * diff;
}

static void solve_constraints_one_pass(World *w) {
    ConstraintPool *cp = &w->constraints;
    ParticlePool   *pp = &w->particles;
    for (int i = 0; i < cp->count; ++i) {
        const Constraint *c = &cp->items[i];
        if (!c->active) continue;
        switch ((ConstraintKind)c->kind) {
            case CSTR_DISTANCE:        solve_distance(pp, c);       break;
            case CSTR_DISTANCE_LIMIT:  solve_distance_limit(pp, c); break;
            case CSTR_ANGLE:           solve_angle(pp, c);          break;
            case CSTR_FIXED_ANCHOR:    solve_fixed_anchor(pp, c);   break;
        }
    }
}

/* ---- Map collision ------------------------------------------------- */

/* For each particle: sample the four tile neighborhoods around it; if
 * any are SOLID and the particle's circle overlaps the tile rectangle,
 * push the particle out along the shortest-axis separation.
 *
 * This is enough for M1's tile-only world. Slopes and free polygons get
 * a closest-point test in a later milestone. */
/* Two flavors of contact:
 *  - position-only (used inside the relaxation iters): pushes pos out of
 *    the surface, leaves prev alone. The body re-equilibrates over the
 *    next constraint iteration.
 *  - position + velocity (used on the final pass): also zeros the
 *    velocity component along the contact normal and applies tangential
 *    friction, so we don't bounce off the floor or coast forever. */
/* Pack (nx, ny) into the per-particle Q1.7 contact-normal slots. Done
 * by both contact flavors so the next tick's mech_step_drive sees the
 * latest contact regardless of which iteration produced it. */
static inline void persist_contact(ParticlePool *p, int i,
                                   float nx, float ny, uint8_t kind) {
    int qx = (int)(nx * 127.0f);
    int qy = (int)(ny * 127.0f);
    if (qx >  127) qx =  127;
    if (qx < -128) qx = -128;
    if (qy >  127) qy =  127;
    if (qy < -128) qy = -128;
    p->contact_nx_q[i] = (int8_t)qx;
    p->contact_ny_q[i] = (int8_t)qy;
    p->contact_kind[i] = kind;
    /* Floor (ny < -0.5) is the GROUNDED case (set elsewhere by the
     * collision caller). Ceiling (ny > 0.5) is the new flag — used by
     * apply_jet_force to slide along angled overhangs. */
    if (ny >  0.5f) p->flags[i] |= PARTICLE_FLAG_CEILING;
}

static inline void contact_position_only(ParticlePool *p, int i,
                                         float nx, float ny, float amount,
                                         uint8_t kind) {
    p->pos_x[i] += nx * amount;
    p->pos_y[i] += ny * amount;
    persist_contact(p, i, nx, ny, kind);
}

static inline void contact_with_velocity(ParticlePool *p, int i,
                                         float nx, float ny, float amount,
                                         uint8_t kind) {
    p->pos_x[i] += nx * amount;
    p->pos_y[i] += ny * amount;
    float vx = p->pos_x[i] - p->prev_x[i];
    float vy = p->pos_y[i] - p->prev_y[i];
    float vn = vx * nx + vy * ny;

    /* Slope-angle-aware friction (P02). Floor (ny ≈ -1): full friction
     * (~0.92). 45° slope (ny ≈ -0.7): less friction (~0.94 — slide a
     * bit). Steep slope (ny ≈ -0.4, 65°): very low friction (~0.97 —
     * slide freely). Combined with slope-tangent run velocity in
     * mech.c, this gives the Soldat-like uphill-slow / downhill-fast
     * feel. */
    float ny_abs = (ny < 0.0f) ? -ny : ny;
    float friction = 0.99f - 0.07f * ny_abs;     /* [0.92 .. 0.99] */
    if (friction > 0.998f) friction = 0.998f;
    if (kind & TILE_F_ICE) friction = 0.998f;

    float vtx = (vx - vn * nx) * friction;
    float vty = (vy - vn * ny) * friction;
    p->prev_x[i] = p->pos_x[i] - vtx;
    p->prev_y[i] = p->pos_y[i] - vty;

    persist_contact(p, i, nx, ny, kind);
}

static void collide_map_one_pass(World *w, bool finalize_velocity) {
    ParticlePool *p  = &w->particles;
    const Level  *L  = &w->level;
    const float   ts = (float)L->tile_size;
    const float   r  = PHYSICS_PARTICLE_RADIUS;

    for (int i = 0; i < p->count; ++i) {
        if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
        /* wan-fixes-3 — kinematic particles (inv_mass == 0) are
         * authoritatively positioned elsewhere (e.g. snapshot_interp_remotes
         * for non-local mechs on the client). Tile collision must NOT
         * displace them; otherwise the body shape drifts asymmetrically
         * (one foot pushed out of a wall, pelvis lerps smoothly, render
         * shows wobbling limbs). Mirrors the inv_mass == 0 early-return
         * in physics_apply_gravity / physics_integrate / solve_distance. */
        if (p->inv_mass[i] <= 0.0f) continue;

        float px = p->pos_x[i];
        float py = p->pos_y[i];
        int   tx = (int)(px / ts);
        int   ty = (int)(py / ts);

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int cx = tx + dx, cy = ty + dy;
                uint16_t tflags = level_flags_at(L, cx, cy);
                if (!(tflags & TILE_F_SOLID)) continue;
                uint8_t  kind   = (uint8_t)(tflags & 0xff);

                float minx = (float)cx * ts;
                float miny = (float)cy * ts;
                float maxx = minx + ts;
                float maxy = miny + ts;

                float qx = (px < minx) ? minx : (px > maxx ? maxx : px);
                float qy = (py < miny) ? miny : (py > maxy ? maxy : py);
                float ddx = px - qx;
                float ddy = py - qy;
                float d2 = ddx * ddx + ddy * ddy;
                if (d2 >= r * r) continue;

                float nx, ny, amount;
                if (d2 > 1e-4f) {
                    float d = sqrtf(d2);
                    nx = ddx / d; ny = ddy / d;
                    amount = r - d;
                    /* If the particle ends up on the opposite side
                     * of the tile from where it came (e.g., constraint
                     * relaxation flicked a body part through a 1-tile
                     * platform), push it back toward the side it
                     * came from instead of further out the wrong side. */
                    float ppx = p->prev_x[i];
                    float ppy = p->prev_y[i];
                    if (ppy > maxy && py < miny) {
                        nx = 0; ny = 1; amount = (maxy - py) + r;
                    } else if (ppy < miny && py > maxy) {
                        nx = 0; ny = -1; amount = (py - miny) + r;
                    } else if (ppx > maxx && px < minx) {
                        nx = 1; ny = 0; amount = (maxx - px) + r;
                    } else if (ppx < minx && px > maxx) {
                        nx = -1; ny = 0; amount = (px - minx) + r;
                    }
                } else {
                    /* Center inside the tile. Pick the exit by where the
                     * particle came from (its prev position is stable
                     * across iteration pushes, since position-only
                     * contacts don't update prev), and only fall back to
                     * a neighbour-priority heuristic when prev was also
                     * inside the tile.
                     *
                     * Without the came-from check, a jet that drives a
                     * particle up into a 1-tile-thick platform from
                     * below gets pushed UP through to the top (open-up
                     * was first in priority), tunneling the body
                     * through the platform and adding to its upward
                     * speed in the process. */
                    bool open_up    = (level_tile_at(L, cx, cy - 1) != TILE_SOLID);
                    bool open_down  = (level_tile_at(L, cx, cy + 1) != TILE_SOLID);
                    bool open_left  = (level_tile_at(L, cx - 1, cy) != TILE_SOLID);
                    bool open_right = (level_tile_at(L, cx + 1, cy) != TILE_SOLID);

                    float ppx = p->prev_x[i];
                    float ppy = p->prev_y[i];
                    bool from_above = ppy < miny;   /* moving down into tile */
                    bool from_below = ppy > maxy;   /* moving up into tile */
                    bool from_left  = ppx < minx;
                    bool from_right = ppx > maxx;

                    if (from_below && open_down) {
                        nx = 0; ny = 1; amount = (maxy - py) + r;
                    } else if (from_above && open_up) {
                        nx = 0; ny = -1; amount = (py - miny) + r;
                    } else if (from_left && open_left) {
                        nx = -1; ny = 0; amount = (px - minx) + r;
                    } else if (from_right && open_right) {
                        nx = 1; ny = 0; amount = (maxx - px) + r;
                    } else if (open_up) {
                        nx = 0; ny = -1; amount = (py - miny) + r;
                    } else if (open_left) {
                        nx = -1; ny = 0; amount = (px - minx) + r;
                    } else if (open_right) {
                        nx = 1; ny = 0; amount = (maxx - px) + r;
                    } else if (open_down) {
                        nx = 0; ny = 1; amount = (maxy - py) + r;
                    } else {
                        /* Surrounded by solids — fall back to shortest. */
                        float d_top = py - miny, d_bot = maxy - py;
                        float d_lft = px - minx, d_rgt = maxx - px;
                        float min_d = d_top; int axis = 2;
                        if (d_lft < min_d) { min_d = d_lft; axis = 0; }
                        if (d_rgt < min_d) { min_d = d_rgt; axis = 1; }
                        if (d_bot < min_d) { min_d = d_bot; axis = 3; }
                        switch (axis) {
                            case 0: nx = -1; ny =  0; amount = d_lft + r; break;
                            case 1: nx =  1; ny =  0; amount = d_rgt + r; break;
                            case 2: nx =  0; ny = -1; amount = d_top + r; break;
                            default: nx =  0; ny =  1; amount = d_bot + r; break;
                        }
                    }

                    SHOT_LOG("t=%llu inside_tile particle=%d tile=(%d,%d) "
                             "pos=(%.1f,%.1f) prev=(%.1f,%.1f) exit=(%.0f,%.0f)",
                             (unsigned long long)w->tick, i, cx, cy,
                             px, py, ppx, ppy, nx, ny);
                }

                if (finalize_velocity) contact_with_velocity(p, i, nx, ny, amount, kind);
                else                   contact_position_only(p, i, nx, ny, amount, kind);
                if (ny < -0.5f) p->flags[i] |= PARTICLE_FLAG_GROUNDED;

                /* Refresh local copy so subsequent neighbours see the push. */
                px = p->pos_x[i];
                py = p->pos_y[i];
            }
        }
    }
}

/* ---- Polygon collision (P02) -------------------------------------- */

/* Closest point on a triangle (a, b, c) to point p. Returns the world-
 * space closest point and writes the index of the touched edge (0/1/2)
 * via *out_edge — used so the caller can read the polygon's pre-baked
 * normal for the push-out direction. Vertex-closest cases (where the
 * closest point is exactly at one of the triangle's corners) set
 * *out_edge = -1; the caller then averages the two adjacent edge normals. */
static Vec2 closest_point_on_tri(Vec2 p,
                                 Vec2 a, Vec2 b, Vec2 c, int *out_edge)
{
    /* Christer Ericke / Eberly. Compute barycentric-region tests
     * against the three vertices (1/3) and three edges (3/6); inside
     * is the seventh region.
     *
     * For small polys (~1-2 tile span) the per-call cost is dominated
     * by the dot-products below; staying inline keeps the hot path
     * tight. */
    Vec2 ab = { b.x - a.x, b.y - a.y };
    Vec2 ac = { c.x - a.x, c.y - a.y };
    Vec2 ap = { p.x - a.x, p.y - a.y };
    float d1 = ab.x * ap.x + ab.y * ap.y;
    float d2 = ac.x * ap.x + ac.y * ap.y;
    if (d1 <= 0.0f && d2 <= 0.0f) { *out_edge = -1; return a; }   /* A vertex */

    Vec2 bp = { p.x - b.x, p.y - b.y };
    float d3 = ab.x * bp.x + ab.y * bp.y;
    float d4 = ac.x * bp.x + ac.y * bp.y;
    if (d3 >= 0.0f && d4 <= d3) { *out_edge = -1; return b; }     /* B vertex */

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        *out_edge = 0;                                            /* AB edge */
        return (Vec2){ a.x + v * ab.x, a.y + v * ab.y };
    }

    Vec2 cp = { p.x - c.x, p.y - c.y };
    float d5 = ab.x * cp.x + ab.y * cp.y;
    float d6 = ac.x * cp.x + ac.y * cp.y;
    if (d6 >= 0.0f && d5 <= d6) { *out_edge = -1; return c; }     /* C vertex */

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        *out_edge = 2;                                            /* CA edge */
        return (Vec2){ a.x + w * ac.x, a.y + w * ac.y };
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        *out_edge = 1;                                            /* BC edge */
        return (Vec2){ b.x + w * (c.x - b.x), b.y + w * (c.y - b.y) };
    }

    /* Inside the triangle. */
    float denom = 1.0f / (va + vb + vc);
    float vv = vb * denom;
    float ww = vc * denom;
    *out_edge = -2;                                               /* inside */
    return (Vec2){ a.x + ab.x * vv + ac.x * ww,
                   a.y + ab.y * vv + ac.y * ww };
}

/* Map a PolyKind to the matching TILE_F_* bit so contact_kind can be
 * read uniformly by the friction code. SOLID has no extra bits set. */
static inline uint8_t poly_kind_to_contact_kind(uint16_t kind) {
    uint8_t k = TILE_F_SOLID;
    switch ((PolyKind)kind) {
        case POLY_KIND_ICE:        k |= TILE_F_ICE;        break;
        case POLY_KIND_DEADLY:     k |= TILE_F_DEADLY;     break;
        case POLY_KIND_ONE_WAY:    k |= TILE_F_ONE_WAY;    break;
        case POLY_KIND_BACKGROUND: k  = TILE_F_BACKGROUND; break;
        case POLY_KIND_SOLID:                              break;
    }
    return k;
}

static void collide_polys_one_pass(World *w, bool finalize_velocity) {
    ParticlePool *pp = &w->particles;
    const Level  *L  = &w->level;
    if (L->poly_count == 0 || !L->poly_grid_off) return;

    const float ts = (float)L->tile_size;
    const float r  = PHYSICS_PARTICLE_RADIUS;
    const int   W  = L->width;
    const int   H  = L->height;

    for (int i = 0; i < pp->count; ++i) {
        if (!(pp->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
        /* wan-fixes-3 — same kinematic gate as collide_map_one_pass. */
        if (pp->inv_mass[i] <= 0.0f) continue;

        float px = pp->pos_x[i];
        float py = pp->pos_y[i];
        int   tx = (int)(px / ts);
        int   ty = (int)(py / ts);
        if (tx < 0 || tx >= W || ty < 0 || ty >= H) continue;

        int cell = ty * W + tx;
        int s    = L->poly_grid_off[cell];
        int e    = L->poly_grid_off[cell + 1];
        for (int k = s; k < e; ++k) {
            int pi = L->poly_grid[k];
            const LvlPoly *poly = &L->polys[pi];
            if ((PolyKind)poly->kind == POLY_KIND_BACKGROUND) continue;

            Vec2 a = { (float)poly->v_x[0], (float)poly->v_y[0] };
            Vec2 b = { (float)poly->v_x[1], (float)poly->v_y[1] };
            Vec2 c = { (float)poly->v_x[2], (float)poly->v_y[2] };

            int edge = -2;
            Vec2 cpt = closest_point_on_tri((Vec2){px, py}, a, b, c, &edge);
            float ddx = px - cpt.x;
            float ddy = py - cpt.y;
            float d2  = ddx * ddx + ddy * ddy;

            if (edge != -2) {
                /* Outside the triangle: contact only if within radius. */
                if (d2 >= r * r) continue;
            }
            /* edge == -2 means the particle's center is INSIDE the
             * triangle — always push out. */

            float nx, ny, amount;
            if (edge == 0 || edge == 1 || edge == 2) {
                /* Use the polygon's pre-baked edge normal — Q1.15. */
                nx = poly->normal_x[edge] / 32767.0f;
                ny = poly->normal_y[edge] / 32767.0f;
                if (d2 > 1e-4f) amount = r - sqrtf(d2);
                else            amount = r;
            } else if (edge == -1) {
                /* Vertex-closest. Average the two adjacent edge
                 * normals (e and (e-1)); for our triangle that's
                 * vertices A→edges 0,2; B→0,1; C→1,2. */
                int e1, e2;
                if      (cpt.x == a.x && cpt.y == a.y) { e1 = 0; e2 = 2; }
                else if (cpt.x == b.x && cpt.y == b.y) { e1 = 0; e2 = 1; }
                else                                   { e1 = 1; e2 = 2; }
                nx = (poly->normal_x[e1] + poly->normal_x[e2]) / (2.0f * 32767.0f);
                ny = (poly->normal_y[e1] + poly->normal_y[e2]) / (2.0f * 32767.0f);
                float nlen = sqrtf(nx * nx + ny * ny);
                if (nlen > 1e-4f) { nx /= nlen; ny /= nlen; }
                else              { nx = 0.0f; ny = -1.0f; }
                if (d2 > 1e-4f) amount = r - sqrtf(d2);
                else            amount = r;
            } else {
                /* Inside the triangle: pick the nearest edge and push
                 * the particle out across it (plus r). closest_point_
                 * on_tri's "interior" return doesn't tell us which
                 * edge is nearest, so do a small per-edge distance
                 * scan here.  Three iters; only fires when a particle
                 * has actually penetrated the polygon. */
                Vec2 verts[3] = { a, b, c };
                float best_d2 = 1e30f;
                int   best_e  = 0;
                Vec2  best_q  = (Vec2){ px, py };
                for (int e = 0; e < 3; ++e) {
                    Vec2 va = verts[e];
                    Vec2 vb = verts[(e + 1) % 3];
                    float lex = vb.x - va.x, ley = vb.y - va.y;
                    float len2 = lex * lex + ley * ley;
                    if (len2 < 1e-6f) continue;
                    float tt = ((px - va.x) * lex + (py - va.y) * ley) / len2;
                    if (tt < 0.0f) tt = 0.0f;
                    if (tt > 1.0f) tt = 1.0f;
                    float qx = va.x + tt * lex;
                    float qy = va.y + tt * ley;
                    float ddx = px - qx, ddy = py - qy;
                    float d2_e = ddx * ddx + ddy * ddy;
                    if (d2_e < best_d2) {
                        best_d2 = d2_e;
                        best_e  = e;
                        best_q  = (Vec2){ qx, qy };
                    }
                }
                nx = poly->normal_x[best_e] / 32767.0f;
                ny = poly->normal_y[best_e] / 32767.0f;
                amount = sqrtf(best_d2) + r;
                /* d2 was 0 (interior); this exit is along the edge
                 * normal so the body lands on the surface, not the
                 * floor. */
                (void)best_q;
            }

            /* ONE_WAY: only block if the particle came from the side
             * the normal points toward. (For a floor-style ramp with
             * normal pointing up, particles coming from above are
             * blocked; particles rising from below pass through.) */
            if ((PolyKind)poly->kind == POLY_KIND_ONE_WAY) {
                float dprev = (pp->prev_x[i] - cpt.x) * nx +
                              (pp->prev_y[i] - cpt.y) * ny;
                if (dprev <= 0.0f) continue;       /* came from passable side */
            }

            uint8_t kind = poly_kind_to_contact_kind(poly->kind);
            if (finalize_velocity) contact_with_velocity(pp, i, nx, ny, amount, kind);
            else                   contact_position_only(pp, i, nx, ny, amount, kind);
            if (ny < -0.5f) pp->flags[i] |= PARTICLE_FLAG_GROUNDED;

            px = pp->pos_x[i];
            py = pp->pos_y[i];
        }
    }
}

void physics_constrain_and_collide(World *w) {
    /* Clear stale grounded + ceiling bits — collide passes will re-assert
     * whichever ones still apply this tick. */
    ParticlePool *p = &w->particles;
    for (int i = 0; i < p->count; ++i)
        p->flags[i] &= (uint8_t)~(PARTICLE_FLAG_GROUNDED | PARTICLE_FLAG_CEILING);

    int iters = PHYSICS_CONSTRAINT_ITERATIONS;
    for (int it = 0; it < iters; ++it) {
        bool last = (it == iters - 1);
        solve_constraints_one_pass(w);
        collide_map_one_pass(w, /*finalize_velocity*/ last);
        collide_polys_one_pass(w, /*finalize_velocity*/ last);
    }
}

/* ---- Ambient zones (P02) ------------------------------------------ */
/*
 * apply_ambient_zone_overrides — called from physics_apply_gravity
 * before the gravity write. WIND zones nudge prev_x/prev_y to inject a
 * velocity push; ZERO_G zones flag particles for the gravity loop to
 * skip. ACID zones do nothing here (mech.c::mech_apply_environmental
 * handles the damage tick; they are fundamentally a per-mech concern).
 */
static void apply_ambient_zone_overrides(World *w, float dt,
                                         uint8_t *zero_g_mask)
{
    if (w->level.ambi_count == 0) return;
    ParticlePool *p = &w->particles;
    for (int z = 0; z < w->level.ambi_count; ++z) {
        const LvlAmbi *a = &w->level.ambis[z];
        if (a->kind != AMBI_WIND && a->kind != AMBI_ZERO_G) continue;
        float minx = (float)a->rect_x;
        float miny = (float)a->rect_y;
        float maxx = minx + (float)a->rect_w;
        float maxy = miny + (float)a->rect_h;
        if (a->kind == AMBI_WIND) {
            float sx = (float)a->dir_x_q / 32767.0f;
            float sy = (float)a->dir_y_q / 32767.0f;
            float st = (float)a->strength_q / 32767.0f;
            float fx = sx * st * dt;     /* delta-prev nudge magnitude */
            float fy = sy * st * dt;
            for (int i = 0; i < p->count; ++i) {
                if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
                float px = p->pos_x[i], py = p->pos_y[i];
                if (px < minx || px > maxx || py < miny || py > maxy) continue;
                p->prev_x[i] -= fx;
                p->prev_y[i] -= fy;
            }
        } else { /* AMBI_ZERO_G */
            if (!zero_g_mask) continue;
            for (int i = 0; i < p->count; ++i) {
                if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
                float px = p->pos_x[i], py = p->pos_y[i];
                if (px < minx || px > maxx || py < miny || py > maxy) continue;
                zero_g_mask[i] = 1;
            }
        }
    }
}

void physics_translate_kinematic_swept(ParticlePool *p, const Level *L,
                                       int i, float dx, float dy) {
    float qx = p->pos_x[i];
    float qy = p->pos_y[i];
    float nx = qx + dx;
    float ny = qy + dy;
    float seg2 = dx * dx + dy * dy;
    if (seg2 > 1.0f) {
        float t = 1.0f;
        if (level_ray_hits(L, (Vec2){qx, qy}, (Vec2){nx, ny}, &t)) {
            float seg_len = sqrtf(seg2);
            float t_clamped = t - (PHYSICS_PARTICLE_RADIUS + 0.5f) / seg_len;
            if (t_clamped < 0.0f) t_clamped = 0.0f;
            SHOT_LOG("pose_sweep clamp particle=%d at=(%.1f,%.1f) "
                     "want=(%.2f,%.2f) t=%.3f -> got=(%.2f,%.2f)",
                     i, qx, qy, dx, dy, t_clamped,
                     dx * t_clamped, dy * t_clamped);
            dx *= t_clamped;
            dy *= t_clamped;
        }
    }
    p->pos_x[i]  += dx; p->pos_y[i]  += dy;
    p->prev_x[i] += dx; p->prev_y[i] += dy;
}

void physics_apply_impulse(ParticlePool *p, int idx, Vec2 imp) {
    if (idx < 0 || idx >= p->count) return;
    p->pos_x[idx] += imp.x;
    p->pos_y[idx] += imp.y;
}
