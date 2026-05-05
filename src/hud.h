#pragma once

#include "world.h"

#include "../third_party/raylib/src/raylib.h"

/* HUD — health bar, ammo counter, crosshair, jet fuel, kill feed line.
 * Drawn in screen space *after* EndMode2D.
 *
 * `camera` is the world-space camera the renderer just used. P07 reads
 * it for the CTF off-screen flag compass: project flag world position
 * to screen, draw an arrow at the nearest edge if the flag is outside
 * the view. */
void hud_draw(const World *w, int screen_w, int screen_h, Vec2 cursor_screen,
              Camera2D camera);
