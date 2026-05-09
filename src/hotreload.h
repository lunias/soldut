#pragma once

/*
 * hotreload — dev-builds-only mtime watcher.
 *
 * Each registered file is stat()'d once every poll cycle (~250 ms). On
 * any mtime change after the initial registration, the registered
 * callback fires with the file's path; the callback's job is to drop
 * the old asset (UnloadFont / UnloadTexture / etc.) and reload from
 * disk.
 *
 * Compile-time gated: outside `DEV_BUILD`, every public function is a
 * no-op so release binaries don't poll the filesystem on every frame.
 * The DBG_CFLAGS in the Makefile defines DEV_BUILD; the release path
 * does not.
 *
 * Usage:
 *
 *   hotreload_register("assets/sprites/trooper.png", reload_chassis);
 *   ...
 *   while (running) {
 *       hotreload_poll();
 *       ...
 *   }
 *
 * v1 limitations (deliberate):
 *   - Per-file registration only. Globbing / directory-walks are
 *     M6 polish.
 *   - The registered set is fixed at startup. Per-map kit textures
 *     (parallax_*.png, tiles.png) reload only when the map changes
 *     (via map_kit_load) — designers iterating on a single map need to
 *     trigger a kit reload via a manual mechanism.
 *   - Callbacks should be fast (LoadTexture is sub-millisecond on a
 *     warm cache); poll runs on the main thread.
 *   - Hard cap of 64 registered entries. Asset count grows; bump if it
 *     bites.
 */

typedef void (*HotReloadCallback)(const char *path);

/* Register a file path + callback. Stores the file's current mtime so
 * the very first poll doesn't fire the callback. Path is copied; the
 * caller can free or reuse the buffer. Idempotent on duplicates (same
 * path → no second registration). */
void hotreload_register(const char *path, HotReloadCallback cb);

/* Drop every registration (used by tests / shutdown). Safe to call
 * when nothing was ever registered. */
void hotreload_clear(void);

/* Stat every registered file. If 250 ms hasn't elapsed since the last
 * poll, this is a single GetTime() check + early return. Otherwise it
 * walks the table and fires callbacks for any file whose mtime
 * advanced. Designed to be called once per frame. */
void hotreload_poll(void);
