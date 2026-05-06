#pragma once

#include <stdint.h>

/*
 * One input bitmask for the whole game. Per-tick the platform layer
 * fills a ClientInput; gameplay reads the bitmask and the analog aim
 * vector.
 *
 * (See [01-philosophy.md] Rule 9: one bitmask, period.)
 */

enum {
    BTN_LEFT     = 1u << 0,
    BTN_RIGHT    = 1u << 1,
    BTN_JUMP     = 1u << 2,
    BTN_JET      = 1u << 3,
    BTN_CROUCH   = 1u << 4,
    BTN_PRONE    = 1u << 5,
    BTN_FIRE     = 1u << 6,
    BTN_RELOAD   = 1u << 7,
    BTN_MELEE    = 1u << 8,
    BTN_USE      = 1u << 9,
    BTN_SWAP     = 1u << 10,
    BTN_DASH     = 1u << 11,
    BTN_FIRE_SECONDARY = 1u << 12,   /* RMB — fires inactive slot one-shot */
    /* 13..15 reserved for future binds */
};

typedef struct {
    uint16_t buttons;       /* BTN_* mask */
    uint16_t seq;           /* monotonic, wraps */
    float    aim_x, aim_y;  /* world space */
    float    dt;            /* tick dt */
} ClientInput;
