#!/usr/bin/env bash
# cross-windows.sh — cross-compile soldut to Windows x86_64 via `zig cc`.
#
# Requires `zig` on PATH. Install:
#   - apt:   sudo snap install zig --classic   (or fetch from ziglang.org)
#   - brew:  brew install zig
#
# Output: build/Soldut.exe + build/Soldut-windows.zip

set -euo pipefail

cd "$(dirname "$0")"

if ! command -v zig >/dev/null 2>&1; then
    echo "error: zig not found on PATH. Install Zig and re-run." >&2
    echo "  https://ziglang.org/download/" >&2
    exit 1
fi

mkdir -p build/windows

ZIG_TARGET=x86_64-windows-gnu

echo "[cross-windows] building raylib for $ZIG_TARGET..."
make -C third_party/raylib/src clean >/dev/null
# Two raylib-meets-zig-cc issues to paper over:
#   1. clang 16+ (which `zig cc` ships) makes
#      -Wincompatible-function-pointer-types a default error; raylib 5.x
#      has a stale ReleaseFileGLTFCallback signature that doesn't match
#      its vendored cgltf's file.release field. Suppress the warning.
#   2. With a Windows target, `zig cc` defaults to writing *.obj, but
#      raylib's Makefile passes a *.o OBJS list to `ar`. The wrapper
#      tools/zigcc-objout injects `-o <name>.o` for compile-only invocations.
make -C third_party/raylib/src PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC \
     CC="$PWD/tools/zigcc-objout -target $ZIG_TARGET" \
     AR="zig ar" \
     OS=Windows_NT \
     CUSTOM_CFLAGS="-Wno-incompatible-function-pointer-types"

echo "[cross-windows] building enet for $ZIG_TARGET..."
rm -f third_party/enet/*.o third_party/enet/libenet.a
for c in third_party/enet/*.c; do
    zig cc -target $ZIG_TARGET -O2 -Ithird_party/enet/include \
        -c "$c" -o "${c%.c}.o"
done
zig ar rcs third_party/enet/libenet.a third_party/enet/*.o

echo "[cross-windows] building soldut..."
mkdir -p build/windows
zig cc -target $ZIG_TARGET \
    -std=c11 -O2 -g -Wall -Wextra -Wpedantic -DNDEBUG \
    -Ithird_party/raylib/src -Ithird_party/enet/include -Ithird_party \
    src/*.c \
    third_party/raylib/src/libraylib.a \
    third_party/enet/libenet.a \
    -lopengl32 -lgdi32 -lwinmm -lws2_32 \
    -o build/windows/Soldut.exe

if command -v zip >/dev/null 2>&1; then
    (cd build/windows && zip -r Soldut-windows.zip Soldut.exe >/dev/null)
fi
echo "[cross-windows] -> build/windows/Soldut.exe"

# Restore native raylib build so subsequent `make` works on the host.
# Local-dev convenience only — skipped on CI, where the runner is
# throwaway and the Windows-cross job doesn't install X11 dev headers
# anyway (so the restoration would fail on rglfw.c).
if [[ -z "${CI:-}" ]]; then
    echo "[cross-windows] restoring host-native raylib build..."
    make -C third_party/raylib/src clean >/dev/null
    make -C third_party/raylib/src PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC \
        CUSTOM_CFLAGS="-Wno-incompatible-function-pointer-types" >/dev/null
    rm -f third_party/enet/*.o third_party/enet/libenet.a
fi
