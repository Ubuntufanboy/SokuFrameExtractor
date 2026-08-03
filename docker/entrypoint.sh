#!/usr/bin/env bash
# Provision a disposable 32-bit Wine prefix, then run the collector.
#
# WHY A CONTAINER-OWNED win32 PREFIX
# ----------------------------------
# The game is a 32-bit binary and the extractor patches its code inline (a
# 5-byte JMP over wglSwapBuffers, plus a vtable overwrite). Both of those are
# sensitive to how Wine runs 32-bit code:
#
#   * A win64 prefix on a new-WoW64 Wine thunks 32-bit code through a 64-bit
#     host process, and the module crashes the game during load (C0000005,
#     before it can log anything).
#   * A win64 prefix cannot be opened by a 32-bit wineserver at all:
#     "is a 64-bit installation, it cannot be used with a 32-bit wineserver".
#
# A pure WINEARCH=win32 prefix on a classic 32-bit Wine avoids both. So the
# container does not reuse the host's prefix -- it builds its own, and the game
# files are mounted in. That also keeps collection reproducible: the prefix is
# disposable and identical on every host.
#
# Usage:
#   docker run --rm \
#     -v /path/to/Soku:/game \                 # the game install (rw)
#     -v "$PWD/out:/out" \                     # captures land here
#     -v sfe-prefix:/wineprefix \              # optional: persist the prefix
#     sfe-collect --out /out --limit 3
set -euo pipefail

export WINEPREFIX="${WINEPREFIX:-/wineprefix}"
export WINEARCH=win32
export WINEDEBUG="${WINEDEBUG:--all}"

GAME_DIR="${GAME_DIR:-/game}"
GAME_LINK="$WINEPREFIX/drive_c/Games/Soku"

if [ ! -d "$GAME_DIR" ]; then
    echo "error: no game mounted at $GAME_DIR" >&2
    echo "       docker run -v /path/to/Soku:/game ..." >&2
    exit 2
fi

# First run: create the prefix. wineboot on a fresh prefix is slow (~30 s) and
# noisy, so only do it once and keep the volume if you care.
if [ ! -f "$WINEPREFIX/system.reg" ]; then
    echo "provisioning 32-bit Wine prefix at $WINEPREFIX ..."
    mkdir -p "$WINEPREFIX"
    wineboot -i >/dev/null 2>&1 || true
    wineserver -w
fi

# NO MSVC redistributable is installed, deliberately.
#
# This used to run `winetricks -q vcrun2019` on first launch, because Wine's
# builtin msvcp140 does not implement everything the MSVC STL calls --
# std::mutex and std::thread reach ?_Throw_Cpp_error@std@@YAXH@Z on their
# failure paths, and Wine aborts the process the moment it is called:
#
#   wine: Call from ... to unimplemented function
#         msvcp140.dll.?_Throw_Cpp_error@std@@YAXH@Z, aborting
#
# The abort happened during module load, so the symptom was a game that died
# with an empty SokuFrameExtractor.log: initLog()'s fopen had succeeded, but
# nothing got as far as writing a line. It looked exactly like "the module
# never loaded", and reading it that way cost hours.
#
# The durable fix was to stop linking the MSVC STL at all -- the module uses
# Win32 primitives (CreateThread, CRITICAL_SECTION, FILE*) directly, which took
# it from 92,672 to 36,352 bytes. cmake/CheckNoMsvcp.cmake fails the build if
# an MSVCP140 import ever reappears, naming the header that reintroduced it.
#
# So this image needs no network on first run, and the prefix needs no
# provisioning beyond wineboot.

arch=$(grep -a '^#arch' "$WINEPREFIX/system.reg" 2>/dev/null | head -1 || true)
case "$arch" in
    *win32*) : ;;
    *) echo "error: $WINEPREFIX is '$arch', expected #arch=win32." >&2
       echo "       Remove the prefix volume and let it be recreated." >&2
       exit 2 ;;
esac

# Point the prefix at the mounted game rather than copying ~5 GB into it.
mkdir -p "$(dirname "$GAME_LINK")"
if [ ! -e "$GAME_LINK" ]; then
    ln -s "$GAME_DIR" "$GAME_LINK"
fi

exec python3 -m runner.collect --prefix "$WINEPREFIX" "$@"
