# Soldut native Makefile.
#
# `make`            — build the game for the host platform.
# `make raylib`     — (re)build third_party/raylib/src/libraylib.a.
# `make enet`       — (re)build third_party/enet/libenet.a.
# `make windows`    — cross-compile to Windows via zig cc (delegates to a script).
# `make macos`      — cross-compile to macOS via zig cc (delegates to a script).
# `make clean`      — remove build artifacts (keeps third_party libs).
# `make distclean`  — also rebuild third_party libs from scratch.
#
# We deliberately don't pull in CMake / Bazel / Meson. See [08-build-and-distribution.md].

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
WARNINGS = -Wno-unused-parameter -Wno-unused-function
INCLUDES = -Ithird_party/raylib/src -Ithird_party/enet/include -Ithird_party

# Detect platform.
UNAME_S := $(shell uname -s 2>/dev/null)

ifeq ($(OS),Windows_NT)
    PLATFORM := windows
    EXE_SUFFIX := .exe
    PLATFORM_LIBS := -lopengl32 -lgdi32 -lwinmm -lws2_32
else ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
    EXE_SUFFIX :=
    PLATFORM_LIBS := -framework OpenGL -framework Cocoa -framework IOKit \
                     -framework CoreAudio -framework CoreVideo -framework CoreFoundation
else
    PLATFORM := linux
    EXE_SUFFIX :=
    PLATFORM_LIBS := -lGL -lpthread -ldl -lrt -lm \
                     -lX11 -lXrandr -lXi -lXinerama -lXcursor
endif

LDFLAGS := -Lthird_party/raylib/src -Lthird_party/enet
LIBS    := -lraylib -lenet $(PLATFORM_LIBS) -lm

BUILD_DIR := build
SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=$(BUILD_DIR)/%.o)
DEP := $(OBJ:.o=.d)

BIN := soldut$(EXE_SUFFIX)

RAYLIB_LIB := third_party/raylib/src/libraylib.a
ENET_LIB   := third_party/enet/libenet.a

.PHONY: all clean distclean raylib enet windows macos help test-physics test-level-io test-spawn test-spawn-e2e test-editor test-pickups test-ctf test-ctf-editor-flow test-grapple-ceiling test-map-share test-map-chunks test-map-registry test-map-placement test-meet-custom test-meet-named test-snapshot test-prefs test-frag-grenade test-riot-cannon-sfx test-mech-ik test-pose-compute test-bot-nav test-bot-playtest host-overlay-preview lobby-overlay-preview title-overlay-preview loadout-preview-overlay summary-overlay-preview bot-tier-preview cook-maps cook-thumbs bake bake-all shot \
        debug gdb gdb-host gdb-client valgrind editor \
        assets-palettes assets-process \
        audio-inventory audio-normalize audio-credits test-audio-smoke \
        perf-bench perf-bench-uncapped perf-bench-stress perf-flamegraph

all: $(BIN)

# ---- Debug build ----------------------------------------------------
# Same source, but compiled with -O0 + ASan/UBSan + extra debug info so
# gdb can walk every local variable. Output binary is `soldut-dbg` —
# distinct from the release binary so you can keep both around.
DBG_CFLAGS = -std=c11 -O0 -g3 -ggdb -Wall -Wextra -Wpedantic -Werror \
             -fno-omit-frame-pointer -fsanitize=address -fsanitize=undefined \
             -DDEV_BUILD
DBG_LDFLAGS = -fsanitize=address -fsanitize=undefined
DBG_OBJ := $(SRC:src/%.c=$(BUILD_DIR)/dbg/%.o)
DBG_DEP := $(DBG_OBJ:.o=.d)
DBG_BIN := soldut-dbg$(EXE_SUFFIX)

$(BUILD_DIR)/dbg:
	@mkdir -p $(BUILD_DIR)/dbg

$(BUILD_DIR)/dbg/%.o: src/%.c | $(BUILD_DIR)/dbg
	$(CC) $(DBG_CFLAGS) $(WARNINGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(DBG_BIN): $(DBG_OBJ) $(RAYLIB_LIB) $(ENET_LIB)
	$(CC) $(DBG_OBJ) $(LDFLAGS) $(DBG_LDFLAGS) $(LIBS) -o $@

debug: $(DBG_BIN)

# `make gdb` launches the release binary under gdb with our init
# script (tools/gdb/init.gdb) preloaded — useful breakpoints + macros.
# `gdb-host` and `gdb-client` skip into the right launch flow.
GDB := gdb -q -x tools/gdb/init.gdb

gdb: $(DBG_BIN)
	$(GDB) ./$(DBG_BIN)

gdb-host: $(DBG_BIN)
	$(GDB) --args ./$(DBG_BIN) --host 23073 --name HostA

# Default to localhost. Override:  make gdb-client HOST=10.0.0.5:23073
HOST ?= 127.0.0.1:23073
gdb-client: $(DBG_BIN)
	$(GDB) --args ./$(DBG_BIN) --connect $(HOST) --name DbgClient

# valgrind catches uninitialized reads + out-of-bounds + leaks. Slow
# (~10x). Useful for the headless physics or shot tests, not for
# interactive play.
valgrind: $(BUILD_DIR)/headless_sim
	valgrind --leak-check=full --show-leak-kinds=definite,indirect \
	         --error-exitcode=2 --track-origins=yes \
	         ./$(BUILD_DIR)/headless_sim

# Headless physics tester — runs simulate() over scripted inputs and
# dumps particle positions. Lets us iterate on physics/pose without
# needing a window. Links every src/ object except main.o.
HEADLESS_OBJ := $(filter-out $(BUILD_DIR)/main.o,$(OBJ))

$(BUILD_DIR)/headless_sim: tests/headless_sim.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/headless_sim.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-physics: $(BUILD_DIR)/headless_sim
	./$(BUILD_DIR)/headless_sim

# .lvl format round-trip + corruption test. Asserts and returns
# non-zero on failure (unlike headless_sim, which is human-read).
$(BUILD_DIR)/level_io_test: tests/level_io_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/level_io_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-level-io: $(BUILD_DIR)/level_io_test
	./$(BUILD_DIR)/level_io_test

# M5 P17 — cook_maps: one-shot exporter that emits the four authored
# .lvl files (Foundry / Slipstream / Reactor / Concourse) under
# assets/maps/. Run once after pulling, or whenever the layout changes.
$(BUILD_DIR)/cook_maps: tools/cook_maps/cook_maps.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tools/cook_maps/cook_maps.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

cook-maps: $(BUILD_DIR)/cook_maps
	./$(BUILD_DIR)/cook_maps

# cook_thumbs — refresh sidecar `<short>_thumb.png` whenever a .lvl
# changes. Unlike cook_maps (which rewrites the 8 builtin .lvls),
# cook_thumbs only reads — safe for the default `make` target. The
# wildcard captures every .lvl on disk (builtins + editor scratch);
# pattern rule rebuilds the matching PNG only when stale, so a clean
# tree generates 8 thumbs and a no-op rebuild stays no-op.
$(BUILD_DIR)/cook_thumbs: tools/cook_thumbs/cook_thumbs.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tools/cook_thumbs/cook_thumbs.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

LVL_FILES   := $(wildcard assets/maps/*.lvl)
THUMB_FILES := $(LVL_FILES:.lvl=_thumb.png)

assets/maps/%_thumb.png: assets/maps/%.lvl $(BUILD_DIR)/cook_thumbs
	./$(BUILD_DIR)/cook_thumbs $<

cook-thumbs: $(THUMB_FILES)

all: $(THUMB_FILES)

# M5 P18 — bake-test harness: headless multi-bot run that drives
# simulate() for `duration_s` seconds and dumps per-map heatmap + CSVs
# + acceptance verdict under build/bake/. Used to validate map layouts
# end-to-end before shipping. `make bake-all` runs it on every .lvl.
# Binary lives at build/bake_runner so the output directory build/bake/
# can share the obvious name.
$(BUILD_DIR)/bake_runner: tools/bake/run_bake.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tools/bake/run_bake.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

bake: $(BUILD_DIR)/bake_runner

bake-all: $(BUILD_DIR)/bake_runner
	./tools/bake/run_all.sh

# M5 P05 — pickup runtime + powerup wire mirror + Burst SMG cadence.
# Asserts and returns non-zero on failure; CI-runnable.
$(BUILD_DIR)/pickup_test: tests/pickup_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/pickup_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-pickups: $(BUILD_DIR)/pickup_test
	./$(BUILD_DIR)/pickup_test

# M5 P10-followup — EntitySnapshot wire codec, specifically the
# `primary_id` field. Round-trips a snapshot where the host's mech
# has its secondary slot active (weapon_id != primary_id) and asserts
# the decoded primary_id matches. Pre-fix this would silently lose
# primary_id on the wire.
$(BUILD_DIR)/snapshot_test: tests/snapshot_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/snapshot_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-snapshot: $(BUILD_DIR)/snapshot_test
	./$(BUILD_DIR)/snapshot_test

# M6 — 2-bone analytic IK unit tests + procedural-pose tests. mech_ik
# is a pure-function module on top of math + world, so it compiles
# headless via HEADLESS_OBJ and asserts on numeric tolerances.
$(BUILD_DIR)/mech_ik_test: tests/mech_ik_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/mech_ik_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-mech-ik: $(BUILD_DIR)/mech_ik_test
	./$(BUILD_DIR)/mech_ik_test

$(BUILD_DIR)/pose_compute_test: tests/pose_compute_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/pose_compute_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-pose-compute: $(BUILD_DIR)/pose_compute_test
	./$(BUILD_DIR)/pose_compute_test

# M6 P05 Phase 1 — per-node visibility precompute (asserts symmetry +
# nonzero edge counts on every shipped map). Drives Phase 2's cover-
# aware engagement positioning.
$(BUILD_DIR)/bot_nav_test: tests/bot_nav_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/bot_nav_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-bot-nav: $(BUILD_DIR)/bot_nav_test
	./$(BUILD_DIR)/bot_nav_test

# M6 P05 — networked-bot smoke test. Hosts ./soldut with 4 elite bots,
# connects one observing client, asserts on log milestones across the
# host + client (bot nav build, CTF role assignment, snapshot sync,
# no crashes). Functional end-to-end check for the future-debug case
# where the user is running playtests with bots on the wire.
test-bot-playtest: soldut
	bash tests/net/run_bot_playtest.sh

# wan-fixes-8 — user-prefs persistence (soldut-prefs.cfg). Round-trips
# a UserPrefs, exercises hand-edited cfg parsing + comment stripping +
# unknown-name fallbacks + atomic-save tmp cleanup.
$(BUILD_DIR)/prefs_test: tests/prefs_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/prefs_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-prefs: $(BUILD_DIR)/prefs_test
	./$(BUILD_DIR)/prefs_test

# wan-fixes-10 — frag-grenade AOE-explosion sync regression. Drives a
# paired-shot 2p test (Client A throws three grenades at Client B on
# Crossfire) against a real dedicated server; asserts both clients
# receive matching NET_MSG_EXPLOSION events at the server's
# authoritative position. -b variant tests the reverse (B throws at
# A) for symmetry. Skipped on CI (needs DISPLAY for the shot
# renderer); used locally to verify the wan-fixes-3 trade-off doesn't
# leak into explosion visuals.
test-frag-grenade: $(BIN)
	bash tests/shots/net/run_frag_grenade.sh
	bash tests/shots/net/run_frag_grenade.sh -b

# M6 Bug B — non-hitscan self-fire SFX suppressed on the firer's
# window. Client fires Riot Cannon (WFIRE_SPREAD) and asserts the
# fire-event sfx log line lands in its own log. Pre-fix: silent fire
# on every non-hitscan weapon for the firer because the FIRE_EVENT
# handler suppressed audio under the same `is_self && from_active_slot`
# gate that suppressed the muzzle sparks — but the predict path only
# played audio for HITSCAN.
test-riot-cannon-sfx: $(BIN)
	bash tests/shots/net/run_riot_cannon_sfx.sh

# wan-fixes-9 — visual preview for the "Starting server..." overlay.
# Opens a real raylib window (needs DISPLAY); saves three PNGs to
# build/shots/host_overlay_*.png at known sweep phases of the
# indeterminate bar. Not run in CI (CI is headless); used by devs to
# verify the overlay LOOKS right after layout / animation tweaks.
$(BUILD_DIR)/host_overlay_preview: tests/host_overlay_preview.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/host_overlay_preview.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

host-overlay-preview: $(BUILD_DIR)/host_overlay_preview
	mkdir -p build/shots
	./$(BUILD_DIR)/host_overlay_preview

# M6 round-shape redesign — visual proof that the new Rounds row in
# host_setup_screen_run fits without overlapping adjacent rows or the
# Back/Start buttons at 720p and 1080p.
$(BUILD_DIR)/host_setup_layout_preview: tests/host_setup_layout_preview.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/host_setup_layout_preview.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

host-setup-layout-preview: $(BUILD_DIR)/host_setup_layout_preview
	mkdir -p build/shots
	./$(BUILD_DIR)/host_setup_layout_preview

# wan-fixes-11 — visual preview for the new Ready Up button (idle +
# ready states) and the match-start loading overlay. Same headless-
# CI-skip rationale as host-overlay-preview. Saves three PNGs into
# build/shots/lobby_overlay_*.png.
$(BUILD_DIR)/lobby_overlay_preview: tests/lobby_overlay_preview.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/lobby_overlay_preview.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

lobby-overlay-preview: $(BUILD_DIR)/lobby_overlay_preview
	mkdir -p build/shots
	./$(BUILD_DIR)/lobby_overlay_preview

# m6-ui-fixes — visual preview for the title screen layout fix (button
# stack no longer crowds the M6 subtitle) and the refreshed CONTROLS
# modal. Headless-CI-skipped same as the other overlay previews.
# Captures: title_overlay_plain.png + title_overlay_controls.png.
$(BUILD_DIR)/title_overlay_preview: tests/title_overlay_preview.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/title_overlay_preview.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

title-overlay-preview: $(BUILD_DIR)/title_overlay_preview
	mkdir -p build/shots
	./$(BUILD_DIR)/title_overlay_preview

# M6 lobby-loadout-preview — visual smoke test for the new pulsing
# [PREVIEW LOADOUT] button + modal. Captures 6 PNGs: one of the lobby
# panel with the button visible (modal closed), and one of the modal
# open per chassis cycling through different loadouts. Lets a reviewer
# eyeball the treadmill, spider chart, description text, and cycle
# controls without spinning up an interactive session.
$(BUILD_DIR)/loadout_preview_overlay: tests/loadout_preview_overlay.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/loadout_preview_overlay.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

loadout-preview-overlay: $(BUILD_DIR)/loadout_preview_overlay
	mkdir -p build/shots
	./$(BUILD_DIR)/loadout_preview_overlay

# Summary screen + map-vote thumbnail preview. Captures one shot via
# the sidecar PNG path and one via the .lvl THMB-lump fallback so a
# reviewer can verify both code paths render the same picture.
$(BUILD_DIR)/summary_overlay_preview: tests/summary_overlay_preview.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/summary_overlay_preview.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

summary-overlay-preview: $(BUILD_DIR)/summary_overlay_preview
	mkdir -p build/shots
	./$(BUILD_DIR)/summary_overlay_preview

# M6 — bot tier chip legibility preview. Stands up a fake lobby with
# one human + 4 bots (one per tier) and captures PNGs at 720/1080/1440
# so the chip text can be checked at every supported display size.
$(BUILD_DIR)/bot_tier_preview: tests/bot_tier_preview.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/bot_tier_preview.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

bot-tier-preview: $(BUILD_DIR)/bot_tier_preview
	mkdir -p build/shots
	./$(BUILD_DIR)/bot_tier_preview

# M5 P07 — CTF runtime: ctf_init_round + ctf_step + ctf_drop_on_death.
# Builds a synthetic level with a Red/Blue flag pair, exercises pickup /
# friendly-return / auto-return / capture / no-carry-touch transitions.
$(BUILD_DIR)/ctf_test: tests/ctf_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/ctf_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-ctf: $(BUILD_DIR)/ctf_test
	./$(BUILD_DIR)/ctf_test

# M5 P04 fix verification — map_spawn_point honors level->spawns when
# the .lvl ships authored SPWN records. Pre-fix, F5 test-play would
# put the player in g_red_lanes[0] regardless of what the editor saved.
$(BUILD_DIR)/spawn_test: tests/spawn_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/spawn_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-spawn: $(BUILD_DIR)/spawn_test
	./$(BUILD_DIR)/spawn_test

# M6 P07 — geometric spawn validator. Loads every shipped .lvl and
# checks each spawn point's body-sample positions against
# level_point_solid. Catches "mech spawned inside the floor" bugs that
# the lane-vs-author logic test (test-spawn) doesn't notice.
$(BUILD_DIR)/spawn_geometry_test: tests/spawn_geometry_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/spawn_geometry_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-spawn-geometry: $(BUILD_DIR)/spawn_geometry_test
	./$(BUILD_DIR)/spawn_geometry_test

# M6 P07 — full simulation-based spawn settle test. For each spawn on
# each shipped map, create a mech at the spawn coords and run 90 sim
# ticks; report any spawn whose mech ends up inside solid, falling
# forever, pushed > 80 px sideways, or never settled. Catches the
# "bot stuck on spawn" class that the static geometry test misses.
$(BUILD_DIR)/spawn_settle_test: tests/spawn_settle_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/spawn_settle_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-spawn-settle: $(BUILD_DIR)/spawn_settle_test
	./$(BUILD_DIR)/spawn_settle_test

# M5 P08 — synth_map writes a .lvl on disk for the map-share end-to-end
# test. Used by tests/net/run_map_share.sh; standalone build target so
# CI can bake it before invoking the wrapper.
$(BUILD_DIR)/synth_map: tests/synth_map.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/synth_map.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

# M5 P08 — chunk reassembly + duplicate-detection unit test. Asserts
# correctness of map_download_apply_chunk against a synthesized chunk
# stream (in-order, out-of-order, duplicates, oversize).
$(BUILD_DIR)/map_chunk_test: tests/map_chunk_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/map_chunk_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-map-chunks: $(BUILD_DIR)/map_chunk_test
	./$(BUILD_DIR)/map_chunk_test

# M5 P08b — runtime map registry: builtins + scan of assets/maps/*.lvl.
# Writes synthetic .lvl files into a tmpdir and exercises the registry
# init path. Asserts and returns non-zero on failure; CI-runnable.
$(BUILD_DIR)/map_registry_test: tests/map_registry_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/map_registry_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-map-registry: $(BUILD_DIR)/map_registry_test
	./$(BUILD_DIR)/map_registry_test

# M6 m6-pickup-placement — loads every shipped .lvl and asserts every
# pickup / spawn / flag center sits in walkable space. Hard-fails the
# cook in tools/cook_maps as well, so a regression can't quietly ship.
$(BUILD_DIR)/placement_test: tests/placement_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/placement_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-map-placement: $(BUILD_DIR)/placement_test
	./$(BUILD_DIR)/placement_test

test-map-share: $(BIN) $(BUILD_DIR)/synth_map
	./tests/net/run_map_share.sh

# M5 P08 — full create-map → host → client-downloads → both-walk-and-meet
# end-to-end. Drives the editor binary in shot mode to author a custom
# .lvl, then runs paired host+client shots with isolated tmpdirs +
# XDG_DATA_HOME so the client genuinely has to download.
test-meet-custom: $(BIN) editor
	./tests/shots/net/run_meet_custom.sh

# M5 P08b — same shape as test-meet-custom but with a NON-RESERVED map
# name (`my_arena`). Proves the registry surfaces user-authored maps in
# the lobby without requiring designers to overwrite a builtin slot.
test-meet-named: $(BIN) editor
	./tests/shots/net/run_meet_named.sh

# End-to-end: editor shotmode authors a .lvl with a platform + spawn,
# `./soldut --test-play <lvl>` loads it, soldut.log records the spawn
# coords AND the post-physics pelvis position. The test grep checks
# both — catches breakage in editor save / level_load decode /
# map_spawn_point / runtime physics integration end-to-end.
test-spawn-e2e: $(BIN) editor
	./tests/spawn_e2e.sh

# Editor shot-mode regression suite — runs every shot script under
# tools/editor/shots/. Each script asserts on doc state at its
# checkpoints; failures bubble up as a non-zero exit.
test-editor: editor
	./build/soldut_editor --shot tools/editor/shots/smoke.shot
	./build/soldut_editor --shot tools/editor/shots/bugs.shot
	./build/soldut_editor --shot tools/editor/shots/poly_triangulation.shot
	./build/soldut_editor --shot tools/editor/shots/validate_failures.shot
	./build/soldut_editor --shot tools/editor/shots/scaling_4k.shot
	./build/soldut_editor --shot tools/editor/shots/pickup_save.shot
	./build/soldut_editor --shot tools/editor/shots/loadout.shot
	./build/soldut_editor --shot tools/editor/shots/loadout_dropdowns.shot
	./build/soldut_editor --shot tools/editor/shots/loadout_mode.shot

# Grapple/swing visual + state regression on a purpose-built map. The
# editor shot programmatically writes assets/maps/grapple_test.lvl
# (ceiling + grapple targets at varied heights), then the game shot
# loads it via the new `load_lvl` shotmode directive and runs the
# fire/swing/retract/chain-refire/release flow. Output goes to
# build/shots/m5_grapple_ceiling/ — eyeball t110_after_retract.png
# to confirm the head sits below the ceiling tile boundary instead
# of inside it.
test-grapple-ceiling: $(BIN) editor
	mkdir -p assets/maps
	./build/soldut_editor --shot tools/editor/shots/grapple_test_map.shot
	./$(BIN) --shot tests/shots/m5_grapple_ceiling.shot

# M5 P07 — full CTF editor → game flow.
# 1. Editor shot programmatically authors a CTF map
#    (floor + walls + 2 spawns + 2 flags) and saves it to
#    assets/maps/ctf_test.lvl.
# 2. Game shot loads the saved .lvl, walks the player into the BLUE
#    flag (touch-driven pickup), then back to the RED home flag
#    (capture fires by the both-flags-home rule). Asserts on log
#    lines: ctf_init_round, ctf: pickup, ctf: capture, score R5/B0.
test-ctf-editor-flow: $(BIN) editor
	mkdir -p assets/maps
	./build/soldut_editor --shot tools/editor/shots/ctf_map.shot
	./$(BIN) --shot tests/shots/m5_ctf_editor_map.shot
	@echo "=== ctf-editor-flow (shot) assertions ==="
	@HOST_LOG="build/shots/m5_ctf_editor_map/m5_ctf_editor_map.log" ; \
	 PASS=0 ; FAIL=0 ; \
	 asrt() { if eval "$$2" ; then echo "PASS: $$1" ; PASS=$$((PASS+1)) ; \
	          else echo "FAIL: $$1" ; FAIL=$$((FAIL+1)) ; fi ; } ; \
	 asrt "level loaded from disk"            "grep -q 'loaded.*assets/maps/ctf_test.lvl' '$$HOST_LOG'" ; \
	 asrt "ctf_init_round populated 2 flags"  "grep -q 'ctf_init_round: flags at RED' '$$HOST_LOG'" ; \
	 asrt "touch-driven pickup fired (BLUE)"  "grep -q 'ctf: pickup flag=1 (team=2)' '$$HOST_LOG'" ; \
	 asrt "capture fired (R5/B0)"             "grep -q 'ctf: capture by mech=0.*score R5/B0' '$$HOST_LOG'" ; \
	 echo ; \
	 echo "== ctf-editor-flow (shot) summary: $$PASS passed, $$FAIL failed ==" ; \
	 if [ "$$FAIL" != "0" ] ; then exit 1 ; fi
	@echo
	@echo "=== ctf-editor-flow (--test-play / F5 path) ==="
	@./tests/test_play_ctf.sh
	@echo
	@echo "=== --mode override (F5 mode picker) ==="
	@./tests/test_play_mode_override.sh
	@echo
	@echo "=== client walks own flag (snapshot quant overflow regression) ==="
	@./tests/shots/net/run_ctf_walk_own_flag.sh

# Shot mode — drive a scripted scene through the real renderer and
# write PNGs. Handy for visual diffs without filming. Override SCRIPT
# to point at a different .shot file.
SCRIPT ?= tests/shots/walk_right.shot
shot: $(BIN)
	./$(BIN) --shot $(SCRIPT)

$(BIN): $(OBJ) $(RAYLIB_LIB) $(ENET_LIB)
	$(CC) $(OBJ) $(LDFLAGS) $(LIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# raylib: only built once unless the source changes. The vendored copy
# may already ship libraylib.a; if so we keep it.
#
# CUSTOM_CFLAGS suppresses -Wincompatible-function-pointer-types entirely:
# clang 16+ (Apple Clang in Xcode 15+, and `zig cc`) makes it a default
# error, but raylib 5.x has a stale ReleaseFileGLTFCallback signature
# that doesn't match its vendored cgltf's file.release field. gcc
# silently ignores unknown -Wno-<flag> names so this is safe on Linux too.
RAYLIB_CFLAGS_OVERRIDE = -Wno-incompatible-function-pointer-types

$(RAYLIB_LIB):
	@echo "[soldut] building raylib for $(PLATFORM)..."
	$(MAKE) -C third_party/raylib/src PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC \
	    CUSTOM_CFLAGS="$(RAYLIB_CFLAGS_OVERRIDE)"

raylib:
	$(MAKE) -C third_party/raylib/src PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC \
	    CUSTOM_CFLAGS="$(RAYLIB_CFLAGS_OVERRIDE)"

ENET_SRC := $(wildcard third_party/enet/*.c)
ENET_OBJ := $(ENET_SRC:.c=.o)

$(ENET_LIB): $(ENET_OBJ)
	$(AR) rcs $@ $(ENET_OBJ)

third_party/enet/%.o: third_party/enet/%.c
	$(CC) -O2 -g -Ithird_party/enet/include -DHAS_SOCKLEN_T -c $< -o $@

enet: $(ENET_LIB)

windows:
	./cross-windows.sh

macos:
	./cross-macos.sh

# M5 P04 — level editor. Builds build/soldut_editor by delegating to
# tools/editor/Makefile. Links a subset of src/ (level_io + arena + log
# + ds + hash) plus raylib + raygui (header-only, vendored at
# third_party/raygui/). Does NOT link mech/physics/net.
editor: $(RAYLIB_LIB)
	$(MAKE) -C tools/editor

# Run an editor shot script. SCRIPT defaults to the smoke test; pass
# any other path with `make editor-shot SCRIPT=...`. The runner writes
# PNG screenshots + a paired .log under build/shots/editor/<scriptname>/
# and exits non-zero on assertion failure.
EDITOR_SHOT_SCRIPT ?= tools/editor/shots/smoke.shot
editor-shot: editor
	./build/soldut_editor --shot $(EDITOR_SHOT_SCRIPT)

# M5 P19 — audio asset inventory + normalize + credits + smoke.
#
# audio-inventory walks src/audio.c::g_sfx_manifest + SERVO_PATH +
# the per-map music/ambient paths (kit short_names mirror cook_maps)
# and prints a CSV of (path, kind, size_bytes, status) plus writes
# tools/audio_inventory/missing.txt with one missing path per line.
# Source of truth is audio.c — the tool exposes nothing of its own;
# new manifest entries surface automatically.
$(BUILD_DIR)/audio_inventory: tools/audio_inventory/run_inventory.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) -Isrc tools/audio_inventory/run_inventory.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

audio-inventory: $(BUILD_DIR)/audio_inventory
	./$(BUILD_DIR)/audio_inventory

# audio-normalize re-encodes every sample under assets/sfx/ +
# assets/music/ to the spec format (22050 Hz mono PCM16 WAV /
# Vorbis q3-q5 mono OGG). Idempotent: a second run skips files
# already in the right shape. Pass ARGS=--loudness to also run the
# loudnorm pass (-16 LUFS SFX / -23 LUFS music / -28 LUFS ambient).
audio-normalize:
	bash tools/audio_normalize/normalize.sh $(ARGS)

# audio-credits diffs assets/sfx/ + assets/music/ + the servo path
# against assets/credits.txt. Fails non-zero if any shipped audio
# file is missing a credits row. Protects CC0 compliance.
audio-credits:
	bash tools/audio_inventory/check_credits.sh

# Smoke: boot a 10-tick scripted run; FAIL if soldut.log contains
# any "audio:.*missing" line. Confirms the inventory matches what
# the runtime actually loads (manifest paths vs disk).
test-audio-smoke: $(BIN)
	bash tests/audio_smoke.sh

# M5 P16 — ImageMagick post-process for non-chassis assets.
#
# Chassis sprites at assets/sprites/<chassis>.png are pre-snapped by
# tools/comfy/extract_gostek.py (palette + halve-then-double round-trip
# baked into the extractor) and do NOT go through this step.
#
# Parallax + decorations + per-map tiles authored under assets/raw/ run
# through Pipeline 7 (per documents/m5/11-art-direction.md): ordered-
# dither halftone + palette remap + optional paper-noise multiply +
# nearest-point halve-then-double round-trip. Output lands at the
# corresponding path under assets/ with raw/ stripped.
#
# `assets-palettes` generates the 8 per-map 2-colour palette PNGs from
# the table in documents/m5/11-art-direction.md §"Two-color print, per
# map". Re-run whenever the palette table changes.
#
# Both targets require ImageMagick (`magick` IM7 or `convert` IM6).
assets-palettes:
	bash tools/build_palettes.sh

assets-process: assets-palettes
	bash tools/process_assets.sh

# M6 P06 — perf bench targets. Each writes a CSV under build/perf/ and
# prints a per-zone median/p95/p99/max summary to stdout at shutdown.
# build/perf/ is gitignored (covered by build/).
$(BUILD_DIR)/perf:
	@mkdir -p $(BUILD_DIR)/perf

perf-bench: $(BIN) $(BUILD_DIR)/perf
	./$(BIN) --bench 30 --bench-csv build/perf/bench.csv \
	         --perf-overlay --window 3440x1440

# A/B against the cap. internal-h 0 = render at window resolution.
perf-bench-uncapped: $(BIN) $(BUILD_DIR)/perf
	./$(BIN) --bench 30 --bench-csv build/perf/bench-uncapped.csv \
	         --perf-overlay --window 3440x1440 --internal-h 0

# Closer to the v1 32-mech reference scenario.
perf-bench-stress: $(BIN) $(BUILD_DIR)/perf
	./$(BIN) --bench 30 --bench-csv build/perf/bench-stress.csv \
	         --perf-overlay --window 3440x1440 \
	         --bench-map citadel --bench-bots 8

# perf record + flamegraph. Linux-only; requires perf + brendangregg/FlameGraph
# on PATH. See tools/perf/README.md.
perf-flamegraph: $(BIN) $(BUILD_DIR)/perf
	./tools/perf/flamegraph.sh build/perf/flamegraph.svg

clean:
	rm -rf $(BUILD_DIR) $(BIN) soldut.exe soldut.log
	rm -f $(ENET_OBJ)
	$(MAKE) -C tools/editor clean

distclean: clean
	$(MAKE) -C third_party/raylib/src clean
	rm -f $(RAYLIB_LIB) $(ENET_LIB)

help:
	@echo "Targets:"
	@echo "  make             native release build for $(PLATFORM)"
	@echo "  make debug       O0 + ASan/UBSan debug build → ./soldut-dbg"
	@echo "  make gdb         launch debug binary under gdb"
	@echo "  make gdb-host    gdb + automatic --host launch"
	@echo "  make gdb-client  gdb + --connect HOST=ip:port"
	@echo "  make valgrind    headless-sim under valgrind (full leak check)"
	@echo "  make raylib      build third_party/raylib/src/libraylib.a"
	@echo "  make enet        build third_party/enet/libenet.a"
	@echo "  make windows     cross-compile to Windows via zig cc"
	@echo "  make macos       cross-compile to macOS via zig cc"
	@echo "  make clean       remove our build artifacts"
	@echo "  make distclean   also rebuild third_party libs"
	@echo "  make shot        run scripted scene → build/shots/*.png"
	@echo "                   override script: make shot SCRIPT=path/to/x.shot"
	@echo "  make editor      build the M5 level editor → build/soldut_editor"
	@echo "  make assets-palettes  generate 8 per-map palette PNGs (foundry/slipstream/...)"
	@echo "  make assets-process   run ImageMagick post on assets/raw/ → assets/"

-include $(DEP)
-include $(DBG_DEP)
