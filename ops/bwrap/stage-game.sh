#!/usr/bin/env bash
# Ship the game, the freshly built module and the runner to a bwrap target.
#
#   ops/bwrap/stage-game.sh anon@192.168.1.130 ~/K0NTR0L-2/SokuFrameExtractor/Soku
#
# WHAT IS LEFT BEHIND, AND WHY
# ----------------------------
# `soku_extract/` is 3.2 GB of previously extracted frames -- an *output* of
# this project that happens to live inside the game folder. `crashes/` is
# minidumps. Neither is needed to run the game, and on the box this was written
# for they are half the free disk.
#
# All 25 SWRSToys modules ship in the tree but SWRSToys.ini enables exactly one
# (SokuFrameExtractor), so the rest are 38 MB of DLLs that are never loaded.
# Copying them anyway would also mean a stray enabled module could change what
# the game does between hosts, which is the sort of difference that is very
# expensive to notice later.
#
# The .dat archives ARE copied, all of them, including th105a.dat -- Soku loads
# the SWR data for the characters and stages it inherits, so a replay can touch
# it whatever the matchup.
set -euo pipefail

TARGET="${1:?usage: stage-game.sh user@host /path/to/Soku [/path/to/reps]}"
GAME_SRC="${2:?usage: stage-game.sh user@host /path/to/Soku [/path/to/reps]}"
REP_SRC="${3:-}"
SSH_KEY="${SFE_SSH_KEY:-$HOME/.ssh/id_ed25519_ai}"
SSH="ssh -i $SSH_KEY"

cd "$(dirname "$0")/../.."
REPO="$PWD"

DLL="$REPO/build/msvc-wine/dll/SokuFrameExtractor.dll"
[ -f "$DLL" ] || { echo "no DLL at $DLL -- cmake --build --preset msvc-wine" >&2; exit 2; }

echo "==> game -> $TARGET:~/sfe-game"
tar -C "$GAME_SRC" -cf - \
    --exclude=soku_extract --exclude=crashes --exclude='modules/*' \
    . \
  | $SSH "$TARGET" "mkdir -p ~/sfe-game && tar -x -C ~/sfe-game"

echo "==> module + ini"
tar -C "$REPO" -cf - \
    --transform='s|build/msvc-wine/dll/||' --transform='s|config/||' \
    build/msvc-wine/dll/SokuFrameExtractor.dll config/sfe.ini \
  | $SSH "$TARGET" "mkdir -p ~/sfe-game/modules/SokuFrameExtractor && \
        tar -x -C ~/sfe-game/modules/SokuFrameExtractor && \
        mv ~/sfe-game/modules/SokuFrameExtractor/sfe.ini \
           ~/sfe-game/modules/SokuFrameExtractor/SokuFrameExtractor.ini"

# docker/entrypoint.sh goes too, and is not duplicated for bwrap: prefix
# creation, the win32 arch check and the game symlink are the same work in both
# sandboxes, and a second copy is a second thing to keep in step.
echo "==> runner code -> $TARGET:~/sfe"
tar -C "$REPO" -cf - --exclude='__pycache__' \
    runner pipeline config ops/bwrap docker/entrypoint.sh \
  | $SSH "$TARGET" "mkdir -p ~/sfe && tar -x -C ~/sfe && \
                    chmod +x ~/sfe/ops/bwrap/* ~/sfe/docker/entrypoint.sh"

if [ -n "$REP_SRC" ]; then
    echo "==> replays -> $TARGET:~/sfe-replays"
    tar -C "$REP_SRC" -cf - . \
      | $SSH "$TARGET" "mkdir -p ~/sfe-replays && tar -x -C ~/sfe-replays"
fi

$SSH "$TARGET" "du -sh ~/sfe-game ~/sfe ~/sfe-replays 2>/dev/null; \
                df -h --output=avail / | tail -1 | sed 's/^/free: /'"
