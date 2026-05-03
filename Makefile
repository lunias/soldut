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

.PHONY: all clean distclean raylib enet windows macos help test-physics

all: $(BIN)

# Headless physics tester — runs simulate() over scripted inputs and
# dumps particle positions. Lets us iterate on physics/pose without
# needing a window. Links every src/ object except main.o.
HEADLESS_OBJ := $(filter-out $(BUILD_DIR)/main.o,$(OBJ))

$(BUILD_DIR)/headless_sim: tests/headless_sim.c $(HEADLESS_OBJ) $(RAYLIB_LIB) $(ENET_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) tests/headless_sim.c $(HEADLESS_OBJ) $(LDFLAGS) $(LIBS) -o $@

test-physics: $(BUILD_DIR)/headless_sim
	./$(BUILD_DIR)/headless_sim

$(BIN): $(OBJ) $(RAYLIB_LIB) $(ENET_LIB)
	$(CC) $(OBJ) $(LDFLAGS) $(LIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARNINGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# raylib: only built once unless the source changes. The vendored copy
# may already ship libraylib.a; if so we keep it.
$(RAYLIB_LIB):
	@echo "[soldut] building raylib for $(PLATFORM)..."
	$(MAKE) -C third_party/raylib/src PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC

raylib:
	$(MAKE) -C third_party/raylib/src PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC

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

clean:
	rm -rf $(BUILD_DIR) $(BIN) soldut.exe soldut.log
	rm -f $(ENET_OBJ)

distclean: clean
	$(MAKE) -C third_party/raylib/src clean
	rm -f $(RAYLIB_LIB) $(ENET_LIB)

help:
	@echo "Targets:"
	@echo "  make             native build for $(PLATFORM)"
	@echo "  make raylib      build third_party/raylib/src/libraylib.a"
	@echo "  make enet        build third_party/enet/libenet.a"
	@echo "  make windows     cross-compile to Windows via zig cc"
	@echo "  make macos       cross-compile to macOS via zig cc"
	@echo "  make clean       remove our build artifacts"
	@echo "  make distclean   also rebuild third_party libs"

-include $(DEP)
