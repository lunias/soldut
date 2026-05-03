#pragma once

/*
 * raylib brings Vector2, Vector3, Vector4, Matrix, plus raymath inline ops.
 * We use those directly — there is one Vec2 in the codebase, and it is
 * raylib's. (Rule 9: one way to do each thing.)
 *
 * This header exists for the handful of helpers raymath doesn't cover and
 * that we'd rather not duplicate at every call site.
 */

#include "../third_party/raylib/src/raylib.h"
#include "../third_party/raylib/src/raymath.h"

#include <math.h>
#include <stdint.h>

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
