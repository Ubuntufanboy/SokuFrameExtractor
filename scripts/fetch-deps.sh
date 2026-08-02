#!/usr/bin/env bash
# Fetch and prepare SokuLib at the exact commit this project builds against.
#
# Not a git submodule: the build also needs a local patch applied on top, and a
# submodule with a dirty working tree is a worse trap than an explicit script.
# The patch gates SokuLib's own test targets behind SOKULIB_BUILD_TESTS -- they
# do not cross-compile, and without it `cmake --build` on the whole tree fails.
#
# The previously-working build depended on that patch existing only in one
# untracked directory on one laptop, which is exactly what this fixes.
set -euo pipefail

REPO="https://github.com/SokuDev/SokuLib.git"
COMMIT="0794becf1f578b32329604483be2112fe0afd4ee"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/SokuLib"
PATCH="$ROOT/cmake/patches/sokulib-optional-tests.patch"

mkdir -p "$ROOT/third_party"

if [ ! -d "$DEST/.git" ]; then
    echo "cloning SokuLib -> $DEST"
    git clone --quiet "$REPO" "$DEST"
fi

git -C "$DEST" fetch --quiet origin "$COMMIT" 2>/dev/null || git -C "$DEST" fetch --quiet origin
git -C "$DEST" checkout --quiet "$COMMIT"
echo "SokuLib at $(git -C "$DEST" rev-parse --short HEAD)"

if git -C "$DEST" apply --check "$PATCH" 2>/dev/null; then
    git -C "$DEST" apply "$PATCH"
    echo "applied $(basename "$PATCH")"
else
    echo "patch already applied (or not applicable)"
fi
