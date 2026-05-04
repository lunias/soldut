# tools/editor/Makefile — soldut_editor (M5 P04).
#
# A standalone executable that links a SUBSET of src/ + raylib + raygui
# (header-only). Same C11 + -Werror as the game, but with the float-
# precision warnings raygui's header trips on suppressed.
#
# The top-level Makefile delegates here via `make editor`.

ROOT       := ../..
SRC_DIR    := $(ROOT)/src
TP         := $(ROOT)/third_party

CC         ?= cc
CFLAGS     ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
# clang on macOS doesn't recognize all the gcc-specific warning names
# we suppress here (e.g. -Wno-stringop-truncation, -Wno-format-truncation,
# -Werror=builtin-declaration-mismatch). -Wno-unknown-warning-option
# tells the compiler to stay quiet about flags it doesn't know about
# instead of erroring under -Werror.
WARN       := -Wno-unknown-warning-option \
              -Wno-unused-parameter -Wno-unused-function \
              -Wno-missing-field-initializers \
              -Wno-error=builtin-declaration-mismatch \
              -Wno-error=implicit-function-declaration \
              -Wno-format-truncation
# Force <math.h> and <stdio.h> to be in scope before any other TU includes
# touch raylib's raymath.h (whose static-inline functions reference fabsf
# etc. without including <math.h> themselves) or raygui.h (same shape).
# Without these, the editor's stricter -Werror=builtin-declaration-mismatch
# trips on every TU that pulls world.h.
PREINC     := -include math.h -include stdio.h
INCLUDES   := -I. -I$(SRC_DIR) -I$(TP) -I$(TP)/raylib/src -I$(TP)/raygui

# raygui.h has scattered switch statements with implicit fall-through,
# unchecked fread / fgets, and intentional cast-truncation. Suppress the
# noisy ones rather than rewriting third-party code.
GUIWARN := -Wno-unknown-warning-option \
           -Wno-implicit-fallthrough -Wno-double-promotion -Wno-cast-qual \
           -Wno-format-nonliteral -Wno-pedantic -Wno-unused-result \
           -Wno-stringop-truncation

UNAME_S    := $(shell uname -s 2>/dev/null)
ifeq ($(OS),Windows_NT)
    EXE        := .exe
    PLAT_LIBS  := -lopengl32 -lgdi32 -lwinmm
else ifeq ($(UNAME_S),Darwin)
    EXE        :=
    PLAT_LIBS  := -framework OpenGL -framework Cocoa -framework IOKit \
                  -framework CoreAudio -framework CoreVideo -framework CoreFoundation
else
    EXE        :=
    PLAT_LIBS  := -lGL -lpthread -ldl -lrt -lm \
                  -lX11 -lXrandr -lXi -lXinerama -lXcursor
endif

BUILD_DIR := $(ROOT)/build/editor
BIN       := $(ROOT)/build/soldut_editor$(EXE)

# Editor source files (this directory).
ED_SRC := main.c doc.c poly.c undo.c view.c palette.c tool.c play.c files.c \
          render.c editor_ui.c validate.c shotmode.c
ED_OBJ := $(ED_SRC:%.c=$(BUILD_DIR)/%.o)

# Subset of src/ we link in. Explicitly LISTED (not wildcard) so
# accidental dependencies on mech/physics/net are caught at link time.
GAME_SUBSET := arena.c log.c hash.c ds.c level_io.c
GAME_OBJ    := $(GAME_SUBSET:%.c=$(BUILD_DIR)/src_%.o)

RAYLIB_LIB  := $(TP)/raylib/src/libraylib.a

.PHONY: all clean

all: $(BIN)

$(BIN): $(ED_OBJ) $(GAME_OBJ) $(RAYLIB_LIB)
	$(CC) $(ED_OBJ) $(GAME_OBJ) -L$(TP)/raylib/src -lraylib $(PLAT_LIBS) -lm -o $@

# Editor TUs. main.c gets RAYGUI_IMPLEMENTATION + the gui-warning
# suppressions; everyone else is normal.
$(BUILD_DIR)/main.o: main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARN) $(GUIWARN) $(PREINC) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/files.o: files.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARN) $(GUIWARN) $(PREINC) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARN) $(PREINC) $(INCLUDES) -c $< -o $@

# Subset of src/ — link-only (the editor doesn't simulate combat). The
# `src_` prefix on the object file name keeps these out of the editor's
# own object pool. doc.c uses arrput/arrfree, which need stb_ds's
# implementation — provided by src/ds.c.
$(BUILD_DIR)/src_%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(WARN) $(PREINC) -I$(SRC_DIR) -I$(TP) -I$(TP)/raylib/src -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN)
