#!/usr/bin/env bash
# cross-macos.sh — cross-compile soldut to macOS (universal arm64+x86_64)
# via `zig cc`.
#
# This requires the macOS SDK headers (CoreAudio, Cocoa). Apple's terms
# don't let us redistribute them, so the path below assumes you've copied
# the SDK from a Mac into tools/macos-sdk/. See [08-build-and-distribution.md].
#
# Output: build/macos/Soldut.app

set -euo pipefail

cd "$(dirname "$0")"

if ! command -v zig >/dev/null 2>&1; then
    echo "error: zig not found on PATH. Install Zig and re-run." >&2
    exit 1
fi

SDK_DIR="${MACOS_SDK_DIR:-tools/macos-sdk}"
if [[ ! -d "$SDK_DIR" ]]; then
    cat >&2 <<EOF
error: macOS SDK not found at $SDK_DIR.

To cross-compile to macOS from a non-Mac host, copy
  /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
from a Mac into $SDK_DIR (or set MACOS_SDK_DIR).

Apple's redistribution terms prevent us from shipping the SDK.
EOF
    exit 1
fi

mkdir -p build/macos

build_arch() {
    local arch="$1" out="$2"
    local zig_target="${arch}-macos"

    echo "[cross-macos] building raylib for $zig_target..."
    make -C third_party/raylib/src clean >/dev/null
    # See note in cross-windows.sh — same stale GLTF callback signature
    # tripping clang 16+'s now-default-error pointer-type check. macOS
    # targets don't need the *.obj→*.o wrapper, but it's a no-op there
    # so we use it anyway to keep the two cross scripts consistent.
    make -C third_party/raylib/src PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC \
         CC="$PWD/tools/zigcc-objout -target $zig_target -isysroot $SDK_DIR" \
         AR="zig ar" \
         CUSTOM_CFLAGS="-Wno-incompatible-function-pointer-types"

    echo "[cross-macos] building enet for $zig_target..."
    rm -f third_party/enet/*.o third_party/enet/libenet.a
    for c in third_party/enet/*.c; do
        zig cc -target $zig_target -isysroot "$SDK_DIR" -O2 \
            -Ithird_party/enet/include -c "$c" -o "${c%.c}.o"
    done
    zig ar rcs third_party/enet/libenet.a third_party/enet/*.o

    echo "[cross-macos] building soldut for $zig_target..."
    zig cc -target $zig_target -isysroot "$SDK_DIR" \
        -std=c11 -O2 -g -Wall -Wextra -Wpedantic -DNDEBUG \
        -Ithird_party/raylib/src -Ithird_party/enet/include -Ithird_party \
        src/*.c \
        third_party/raylib/src/libraylib.a \
        third_party/enet/libenet.a \
        -framework OpenGL -framework Cocoa -framework IOKit \
        -framework CoreAudio -framework CoreVideo -framework CoreFoundation \
        -o "$out"
}

build_arch aarch64 build/macos/Soldut-arm64
build_arch x86_64  build/macos/Soldut-x86_64

if command -v lipo >/dev/null 2>&1; then
    echo "[cross-macos] lipo combine..."
    lipo -create -output build/macos/Soldut \
        build/macos/Soldut-arm64 build/macos/Soldut-x86_64
else
    echo "[cross-macos] note: lipo unavailable on host; ship per-arch binaries"
    cp build/macos/Soldut-arm64 build/macos/Soldut
fi

echo "[cross-macos] packaging Soldut.app..."
APP=build/macos/Soldut.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp build/macos/Soldut "$APP/Contents/MacOS/Soldut"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>           <string>Soldut</string>
  <key>CFBundleIdentifier</key>           <string>org.soldut.Soldut</string>
  <key>CFBundleName</key>                 <string>Soldut</string>
  <key>CFBundleVersion</key>              <string>0.0.1</string>
  <key>CFBundleShortVersionString</key>   <string>0.0.1</string>
  <key>CFBundlePackageType</key>          <string>APPL</string>
  <key>LSMinimumSystemVersion</key>       <string>11.0</string>
  <key>NSHighResolutionCapable</key>      <true/>
</dict>
</plist>
EOF

echo "[cross-macos] -> $APP"

# Restore native build artifacts.
make -C third_party/raylib/src clean >/dev/null
make -C third_party/raylib/src PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC >/dev/null
rm -f third_party/enet/*.o third_party/enet/libenet.a
