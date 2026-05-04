/*
 * cook_maps — one-shot exporter that writes the M4 code-built maps
 * out as `.lvl` files. STUB at P01; populated by P17.
 *
 * Goal at completion (P17): for each MapId in {MAP_FOUNDRY,
 * MAP_SLIPSTREAM, MAP_REACTOR}, allocate a synthetic World, call the
 * existing build_foundry / build_slipstream / build_reactor path,
 * populate spawn points + meta, then call level_save() to
 * `assets/maps/<short>.lvl`. The runtime then loads them via
 * level_io and the code-built fallback in src/maps.c becomes dead
 * code (kept for fresh-checkout boot until M6 cleanup).
 *
 * Build (P17): the prompt will add a Makefile target like
 *   tools/cook_maps/cook_maps: tools/cook_maps/cook_maps.c $(HEADLESS_OBJ)
 * with the same shape as the level_io_test target.
 *
 * Until then, this file is intentionally a no-op stub. We don't add it
 * to the default `make` build because nothing depends on it yet.
 */

#include <stdio.h>

int main(void) {
    fprintf(stderr,
        "cook_maps: not implemented yet (stub at P01).\n"
        "TODO (P17): export the three M4 code-built maps as .lvl:\n"
        "  - MAP_FOUNDRY    -> assets/maps/foundry.lvl\n"
        "  - MAP_SLIPSTREAM -> assets/maps/slipstream.lvl\n"
        "  - MAP_REACTOR    -> assets/maps/reactor.lvl\n"
        "P17 will allocate a World per map, call build_foundry/etc.,\n"
        "populate META + SPWN sections, then level_save() each one.\n");
    return 1;
}
