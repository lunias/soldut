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

.PHONY: all clean distclean raylib enet windows macos help test-physics test-level-io test-spawn test-spawn-e2e test-editor shot \
        debug gdb gdb-host gdb-client valgrind editor

all: $(BIN)

# ---- Debug build ----------------------------------------------------
# Same source, but compiled with -O0 + ASan/UBSan + extra debug info so
# gdb can walk every local variable. Output binary is `soldut-dbg` —
# distinct from the release binary so you can keep both around.
DBG_CFLAGS = -std=c11 -O0 -g3 -ggdb -Wall -Wextra -Wpedantic -Werror \
             -fno-omit-frame-pointer -fsanitize=address -fsanitize=undefined
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

# M5 P04 fix verification — map_spawn_point honors level->spawns when
# the .lvl ships authored SPWN records. Pre-fix, F5 test-play would
# put the player in g_red_lanes[0] regardless of what the editor saved.
$(BUILD_DIR)/spawn_test: tests/spawn_test.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/spawn_test.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-spawn: $(BUILD_DIR)/spawn_test
	./$(BUILD_DIR)/spawn_test

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

-include $(DEP)
-include $(DBG_DEP)
