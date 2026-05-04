#pragma once

/*
 * raylib brings Vector2, Vector3, Vector4, Matrix, plus raymath inline ops.
 * We use those directly — there is one Vec2 in the codebase, and it is
 * raylib's. (Rule 9: one way to do each thing.)
 *
 * This header exists for the handful of helpers raymath doesn't cover and
 * that we'd rather not duplicate at every call site.
 */

/* <math.h> first so raymath.h's static-inline functions see fabsf /
 * fmaxf / etc. as proper declarations rather than builtins-without-
 * prototypes. The game's compile happens to tolerate the reverse order
 * (raymath gets a single touch and its inlines aren't all instantiated
 * for every TU); the editor's stricter raygui pull does instantiate
 * them all, which trips -Werror=builtin-declaration-mismatch without
 * this. */
#include <math.h>
#include <stdint.h>

/* Apple's <math.h> on Xcode 16+ hides the C99 float versions of math
 * functions (floorf, sqrtf, fabsf, etc.) under feature-test macros
 * that -std=c11 / -std=gnu11 don't trigger reliably across all CI
 * targets. raymath.h's static-inline functions reference them
 * unconditionally, so we force-declare them here. The `extern "C"`
 * dance isn't needed because soldut is C-only.
 *
 * gcc on Linux + clang on Linux (zig cc) already see these via the
 * <math.h> include above; the redundant decls are harmless. */
/* Float versions — used by raymath.h's static-inline functions and
 * by the editor's polygon math. */
extern float floorf(float);
extern float ceilf (float);
extern float roundf(float);
extern float sqrtf (float);
extern float fabsf (float);
extern float fmaxf (float, float);
extern float fminf (float, float);
extern float sinf  (float);
extern float cosf  (float);
extern float tanf  (float);
extern float atan2f(float, float);
extern float asinf (float);
extern float acosf (float);
extern float expf  (float);
extern float logf  (float);
extern float powf  (float, float);

/* Double versions — used by poly.c (fabs, sqrt) and by raymath.h's
 * `tan(fovY*0.5)` projection helper. */
extern double floor(double);
extern double ceil (double);
extern double round(double);
extern double sqrt (double);
extern double fabs (double);
extern double fmax (double, double);
extern double fmin (double, double);
extern double sin  (double);
extern double cos  (double);
extern double tan  (double);
extern double atan2(double, double);
extern double asin (double);
extern double acos (double);

#include "../third_party/raylib/src/raylib.h"
#include "../third_party/raylib/src/raymath.h"

typedef Vector2 Vec2;

#ifndef PI
#define PI 3.14159265358979323846f
#endif

static inline float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static inline int clampi(int x, int lo, int hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline float saturatef(float x) {
    return clampf(x, 0.0f, 1.0f);
}

static inline float sign_f(float x) {
    return (x > 0.0f) - (x < 0.0f);
}

static inline float vec2_length_sq(Vec2 v) {
    return v.x * v.x + v.y * v.y;
}

static inline Vec2 vec2_make(float x, float y) {
    return (Vec2){x, y};
}
