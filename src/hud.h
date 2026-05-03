#pragma once

#include "world.h"

/* HUD — health bar, ammo counter, crosshair, jet fuel, kill feed line.
 * Drawn in screen space *after* EndMode2D. */

void hud_draw(const World *w, int screen_w, int screen_h, Vec2 cursor_screen);
