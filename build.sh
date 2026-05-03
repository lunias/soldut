#!/usr/bin/env bash
# build.sh — one-shot orchestration for a native build.
#
#   ./build.sh                   # native release build
#   ./build.sh dev               # native debug build with sanitizers
#   ./build.sh windows           # cross to Windows via zig cc
#   ./build.sh macos             # cross to macOS via zig cc (needs SDK)
#
# This wraps `make` for the common cases. Reach for `make` directly when
# you need a single target.

set -euo pipefail

cd "$(dirname "$0")"

mode="${1:-release}"

case "$mode" in
    release)
        echo "[soldut] release build for the host platform"
        make CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Wpedantic -DNDEBUG"
        ;;
    dev|debug)
        echo "[soldut] dev build (asserts on, sanitizers on)"
        make CFLAGS="-std=c11 -O0 -g -Wall -Wextra -Wpedantic -DDEV_BUILD=1 \
                     -fsanitize=address,undefined" \
             LDFLAGS="-fsanitize=address,undefined -Lthird_party/raylib/src -Lthird_party/enet"
        ;;
    windows)
        ./cross-windows.sh
        ;;
    macos)
        ./cross-macos.sh
        ;;
    clean)
        make clean
        ;;
    distclean)
        make distclean
        ;;
    *)
        echo "usage: $0 [release|dev|windows|macos|clean|distclean]" >&2
        exit 1
        ;;
esac

echo "[soldut] done."
