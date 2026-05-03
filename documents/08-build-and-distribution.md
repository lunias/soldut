# 08 — Build & Distribution

This document specifies how the source becomes a binary that lands on a player's machine. The builds must be **reproducible**, **fast**, and **boring** — boring meaning no surprise dependencies, no proprietary tooling, no manual step that someone could forget.

## Targets

| Platform | Architecture | Toolchain |
|---|---|---|
| Windows | x86_64 | mingw-w64 (cross-compile from Linux) OR `zig cc` |
| macOS | arm64 + x86_64 (universal binary) | clang via `zig cc` (cross-compile) OR Xcode SDK on a Mac (signing host) |
| Linux | x86_64 | gcc / clang (native) — for development |

Linux is a **dev platform**; we don't ship a Linux build at v1. We will if community asks.

## Toolchain

### Compiler

**`zig cc`** for cross-compile, native gcc/clang for development.

`zig` (the language and toolchain) ships clang under the hood with **all libc variants bundled** (musl, glibc, msvcrt for mingw, mingw-w64 stubs). One install gives you cross-compile to any platform Zig supports.

```bash
# Cross-compile to Windows from Linux:
zig cc -target x86_64-windows-gnu -Iraylib/include -lraylib -lopengl32 -lgdi32 -lwinmm \
    src/*.c -o build/Soldut.exe

# Cross-compile to macOS arm64 from Linux:
zig cc -target aarch64-macos -Iraylib/include -lraylib \
    -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework CoreVideo \
    src/*.c -o build/Soldut-arm64
```

**Caveat:** cross-compiling to macOS requires Apple's SDK headers (CoreAudio, Cocoa). Apple's redistribution policy means we can't ship the SDK with our build script. We document the manual step: download Xcode CLI tools on a Mac, copy the SDK to `tools/macos-sdk/` on the build host. (Alternatively: build macOS releases on a Mac.)

### Build system

**A `build.sh` shell script** plus a top-level `Makefile`. No CMake at v1. No Bazel. No Meson. No autotools. Reasons:

- We have **one** project, **one** target structure, **two** ship platforms. CMake's value scales with target multiplicity; we don't have it.
- A 50-line `Makefile` is readable; a 50-line `CMakeLists.txt` is half-readable; a 200-line `CMakeLists.txt` is unreadable.
- New developers should be able to `make` and have it work in 10 seconds.

```makefile
# Makefile (sketch)
CC ?= cc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
INCLUDES = -Ithird_party/raylib/src -Ithird_party
LIBS = -lraylib -lm

ifeq ($(OS),Windows_NT)
    LIBS += -lopengl32 -lgdi32 -lwinmm -lws2_32
else ifeq ($(shell uname),Darwin)
    LIBS += -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework CoreVideo
else
    LIBS += -lGL -lpthread -ldl -lrt -lX11
endif

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)

soldut: $(OBJ)
    $(CC) $(OBJ) -o $@ $(LIBS)

build/%.o: src/%.c
    $(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
    rm -rf build/ soldut soldut.exe

.PHONY: clean
```

Plus `cross-windows.sh`, `cross-macos.sh`, `cross-all.sh` for the cross builds.

### Build flags

Development:
- `-std=c11 -O0 -g -Wall -Wextra -Wpedantic -Werror`
- `-fsanitize=address,undefined` (optional, slow)
- `-DDEV_BUILD=1` (enables debug overlays, hot reload, asserts)

Release:
- `-std=c11 -O2 -Wall -Wextra -Wpedantic -Werror`
- `-DNDEBUG`
- LTO disabled at v1 (linker correctness > 5% speed)
- Strip symbols for shipping binaries (`strip Soldut.exe`)

## raylib build

We vendor raylib at `third_party/raylib/`. We build it once per platform as a static library:

```bash
cd third_party/raylib/src
make PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC
# yields libraylib.a
```

For cross-compile we point raylib's Makefile at the cross compiler:

```bash
make PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC \
     CC=x86_64-w64-mingw32-gcc OS=Windows_NT
```

`libraylib.a` is committed to the repo per platform under `third_party/raylib/lib/{linux,windows,macos-arm64,macos-x86_64}/`. Yes, we commit binaries — they're <2 MB each, they don't change often, and reproducible builds without checked-in libs is a six-week side quest.

## stb headers

We commit the stb headers we use directly (`third_party/stb_ds.h`, `stb_sprintf.h`, optionally `stb_leakcheck.h`). raylib already embeds the rest internally (`stb_image`, `stb_truetype`, `stb_vorbis`, etc.) so we don't double-vendor.

## ENet

We vendor ENet at `third_party/enet/`. Build as a static lib alongside our main build:

```bash
cd third_party/enet
gcc -O2 -c *.c -I include
ar rcs libenet.a *.o
```

ENet is ~5k LOC, builds in 1 second. Linked statically.

## Build matrix and CI

A GitHub Actions workflow (or equivalent) runs:

| Job | OS | Output |
|---|---|---|
| `build-linux` | ubuntu-latest | `Soldut-linux-x86_64` (dev binary) |
| `build-windows` | ubuntu-latest (cross via zig cc) | `Soldut-windows-x86_64.exe` |
| `build-macos` | macos-latest (native) | `Soldut.app` (universal, signed if cert in secrets) |
| `test` | ubuntu-latest | runs unit tests |

A successful CI run produces all three artifacts plus a checksum file.

## macOS code signing & notarization

We pay the **$99/year Apple Developer program fee** for the Developer ID certificate. The cert lives in the macOS keychain on the signing host.

```bash
# Sign:
codesign --deep --force --options=runtime --timestamp \
    -s "Developer ID Application: <Name> (<TEAMID>)" \
    Soldut.app

# Notarize:
xcrun notarytool submit Soldut.zip \
    --apple-id "$APPLE_ID" --team-id "$TEAM_ID" --password "$APP_PASS" --wait

# Staple the ticket:
xcrun stapler staple Soldut.app
```

Hardened runtime (`--options=runtime`) is required for notarization. We do not need any entitlements (no JIT, no microphone, no camera).

For a player who downloads our DMG/zip:
- **Signed + notarized + stapled**: opens with a single Gatekeeper prompt (or none, if the user trusts the developer).
- **Signed + notarized but not stapled**: requires online check on first launch.
- **Unsigned**: requires the user to right-click → Open and accept the warning.

We ship signed + notarized + stapled.

### Universal binary

```bash
# Build separately for each arch:
zig cc -target aarch64-macos ... -o build/Soldut-arm64
zig cc -target x86_64-macos ...  -o build/Soldut-x86_64

# Combine:
lipo -create -output Soldut build/Soldut-arm64 build/Soldut-x86_64

# Wrap into .app bundle:
mkdir -p Soldut.app/Contents/MacOS Soldut.app/Contents/Resources
cp Soldut Soldut.app/Contents/MacOS/
cp Info.plist Soldut.app/Contents/
cp -r assets/ Soldut.app/Contents/Resources/

# Sign + notarize as above.
```

## Windows packaging

```bash
# Build:
zig cc -target x86_64-windows-gnu ... -o build/Soldut.exe

# Strip:
x86_64-w64-mingw32-strip build/Soldut.exe

# Package:
mkdir Soldut-windows/
cp build/Soldut.exe Soldut-windows/
cp -r assets/ Soldut-windows/
echo "Soldut.exe is the game. Double-click to play." > Soldut-windows/README.txt
zip -r Soldut-windows.zip Soldut-windows/
```

We do **not** sign the Windows binary at v1. Windows SmartScreen will warn first-time users, who must click "More info → Run anyway." When we have $300/year and the bandwidth, we add an Authenticode cert and sign.

## Asset packaging

Assets live next to the binary:

```
Soldut.exe (or Soldut.app/Contents/MacOS/Soldut)
assets/
    sfx/...
    music/...
    fonts/...
    sprites/...
    maps/...
```

We do **not** pack assets into a zip or pak file at v1 (no compression, no obscurity benefit, harder hot reload). Mods can drop into `assets/` directly. This is the Soldat tradition we copy.

Total ship size target: **<50 MB** including all assets. raylib + ENet libs are <2 MB combined; the rest is art and audio.

## Build performance

| Build | Cold | Incremental |
|---|---|---|
| Game source (~50 .c files, ~30k LOC) | <10 s | <1 s |
| raylib static lib | <30 s | (rare) |
| Full release pipeline (build + sign + notarize) | <8 min | n/a |

If cold build exceeds 10 seconds we stop and fix. Slow builds are the iteration-speed killer Jonathan Blow has been right about for a decade.

## Hot reload

Code does not hot reload at v1. We compile in <1 second; restarting the game is fine. Asset hot reload (textures, sounds, levels) is in the engine — see [06-rendering-audio.md](06-rendering-audio.md).

If we ever need code hot reload, the path is **dlopen the gameplay layer as a shared lib** with a stable C ABI between platform layer and gameplay (Casey Muratori's Handmade Hero pattern). Out of scope at v1.

## Versioning

`version.h` declares:

```c
#define SOLDUT_VERSION_MAJOR 0
#define SOLDUT_VERSION_MINOR 1
#define SOLDUT_VERSION_PATCH 0
#define SOLDUT_VERSION_STRING "0.1.0"
#define SOLDUT_PROTOCOL_ID    0x53304C44  // 'S0LD'
```

Connection handshake includes the `PROTOCOL_ID` and version. Mismatched protocol IDs reject the connection with a clear message ("Client version 0.1.0 incompatible with server 0.2.0").

We bump:
- `PATCH` on bug fixes that don't change netcode or map format.
- `MINOR` when netcode changes (must bump `PROTOCOL_ID`) or significant features are added.
- `MAJOR` for ship milestones.

## Development setup

A new developer:

```bash
git clone https://github.com/your-org/soldut.git
cd soldut
make             # 10 seconds
./soldut         # launch
```

That's the bar. If anyone needs more than three commands, the build system has failed.

For Windows cross-compile from a Linux dev machine:

```bash
brew install zig   # or apt install zig (when packaged)
make windows       # cross-compiles
```

## Distribution channels

At v1: **direct download from a website**. itch.io is the natural starting point — zero friction, supports Windows/macOS, optional pay-what-you-want. We add Steam later if the player base demands it (Steamworks integration is non-trivial; we'd then plug Valve GameNetworkingSockets + Steam Datagram Relay in, behind our existing `net.h` interface).

Update story: a tiny version-check on launch (HTTP GET to a JSON endpoint), prompt the user to download the new version, exit. No auto-update infrastructure at v1.

## What we are NOT doing

- **No CMake.** Maybe ever. Certainly not at v1.
- **No Bazel, Meson, Buck, scons, premake, Tup.** A Makefile is enough.
- **No Docker.** We don't need reproducible build environments badly enough to add a container layer.
- **No vcpkg, conan, brew dependencies for the build.** Our deps are vendored in `third_party/`.
- **No CI matrix for 30 compiler versions.** Three jobs (Linux native, Windows cross, macOS native) is the matrix.
- **No automatic crash reporting at v1.** Players can attach a log file when reporting issues.
- **No telemetry.** We don't phone home.
- **No installer at v1.** Users unzip and run. Installers come if a platform demands it (Windows Store does; we don't ship there).

## References

- raylib build instructions: `third_party/raylib/README.md`.
- Apple — *Notarizing macOS Software Before Distribution*.
- Apple — *Signing Mac Software with Developer ID*.
- `zig cc` cross-compile docs: ziglang.org/learn / `zig.news` archive.
- Casey Muratori — *Loading Game Code Dynamically* (Handmade Hero) for the future-hot-reload reference.

See [reference/sources.md](reference/sources.md) for URLs.
