#include "physics.h"

#include "level.h"
#include "log.h"

#include <math.h>
#include <string.h>

/* ---- Gravity ------------------------------------------------------- */

void physics_apply_gravity(World *w, float dt) {
    ParticlePool *p = &w->particles;
    Vec2 g = w->level.gravity;
    /* Verlet absorbs accelerations as `+ a*dt^2` inside the integrate
     * step. We apply a velocity nudge here by displacing pos directly,
     * which accumulates into the (pos - prev) term on the next integrate.
     *
     * That's equivalent and lets us layer per-frame forces (jet, run,
     * recoil) the same way without double-bookkeeping a velocity field. */
    float fx = g.x * dt * dt;
    float fy = g.y * dt * dt;
    for (int i = 0; i < p->count; ++i) {
        if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
        if (p->inv_mass[i] <= 0.0f) continue;
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
static inline void contact_position_only(ParticlePool *p, int i,
                                         float nx, float ny, float amount) {
    p->pos_x[i] += nx * amount;
    p->pos_y[i] += ny * amount;
}

static inline void contact_with_velocity(ParticlePool *p, int i,
                                         float nx, float ny, float amount) {
    p->pos_x[i] += nx * amount;
    p->pos_y[i] += ny * amount;
    float vx = p->pos_x[i] - p->prev_x[i];
    float vy = p->pos_y[i] - p->prev_y[i];
    float vn = vx * nx + vy * ny;
    float vtx = (vx - vn * nx) * 0.92f;     /* tangential friction */
    float vty = (vy - vn * ny) * 0.92f;
    p->prev_x[i] = p->pos_x[i] - vtx;
    p->prev_y[i] = p->pos_y[i] - vty;
}

static void collide_map_one_pass(World *w, bool finalize_velocity) {
    ParticlePool *p  = &w->particles;
    const Level  *L  = &w->level;
    const float   ts = (float)L->tile_size;
    const float   r  = PHYSICS_PARTICLE_RADIUS;

    for (int i = 0; i < p->count; ++i) {
        if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;

        float px = p->pos_x[i];
        float py = p->pos_y[i];
        int   tx = (int)(px / ts);
        int   ty = (int)(py / ts);

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int cx = tx + dx, cy = ty + dy;
                if (level_tile_at(L, cx, cy) != TILE_SOLID) continue;

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
                }

                if (finalize_velocity) contact_with_velocity(p, i, nx, ny, amount);
                else                   contact_position_only(p, i, nx, ny, amount);
                if (ny < -0.5f) p->flags[i] |= PARTICLE_FLAG_GROUNDED;

                /* Refresh local copy so subsequent neighbours see the push. */
                px = p->pos_x[i];
                py = p->pos_y[i];
            }
        }
    }
}

void physics_constrain_and_collide(World *w) {
    /* Clear stale grounded bits — collide passes will re-assert them. */
    ParticlePool *p = &w->particles;
    for (int i = 0; i < p->count; ++i)
        p->flags[i] &= (uint8_t)~PARTICLE_FLAG_GROUNDED;

    int iters = PHYSICS_CONSTRAINT_ITERATIONS;
    for (int it = 0; it < iters; ++it) {
        bool last = (it == iters - 1);
        solve_constraints_one_pass(w);
        collide_map_one_pass(w, /*finalize_velocity*/ last);
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
