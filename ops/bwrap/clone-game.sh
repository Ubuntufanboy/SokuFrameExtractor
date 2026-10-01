#!/usr/bin/env bash
# Give a worker its own game directory, sharing only the read-only archives.
#
#   ops/bwrap/clone-game.sh ~/sfe-game ~/sfe-game1
#
# ONE GAME DIRECTORY CANNOT SERVE TWO WORKERS
# -------------------------------------------
# `runner/collect.py` writes a per-job SokuFrameExtractor.ini into
# modules/SokuFrameExtractor/ naming that job's FIFO, output directory and
# staged replay. SWRSToys writes currentModule.txt beside the executable. Two
# collectors bound to one game directory therefore overwrite each other's
# configuration between launches.
#
# The failure is quiet and slow rather than loud: the second worker's game
# reads the first worker's ini, writes its frames into the first worker's FIFO,
# and the second worker's FFmpeg sits on a pipe nothing ever opens until the
# per-replay timeout expires. Observed exactly once here -- worker 1 finished
# six replays while worker 0 hung on its first with
# `OutputDir = Z:\out\5263016-...`, an id worker 0 had never heard of.
#
# runner/swarm.py already solves this by cloning the game into each prefix;
# ops/bwrap/sfe-bwrap binds one directory in, so the clone has to happen out
# here instead.
#
# WHY THIS IS CHEAP
# -----------------
# 2.07 GB of the 2.1 GB is five .dat archives the game only ever reads, so they
# are hardlinked and every clone after the first costs about 50 MB. Nothing
# writes to them; if that ever changes, an in-place write through a hardlink
# would corrupt every worker at once, which is why the link list is explicit
# and short rather than "everything that looks read-only".
set -euo pipefail

SRC="${1:?usage: clone-game.sh <src-game-dir> <dst-game-dir>}"
DST="${2:?usage: clone-game.sh <src-game-dir> <dst-game-dir>}"

[ -f "$SRC/th123.exe" ] || { echo "no game at $SRC" >&2; exit 2; }
if [ -f "$DST/th123.exe" ]; then
    echo "clone-game: $DST already exists, leaving it alone"
    exit 0
fi

mkdir -p "$DST"
# Copy everything except the archives...
tar -C "$SRC" -cf - --exclude='*.dat' . | tar -x -C "$DST"
# ...then hardlink those. `cp -l` fails across filesystems, which is the one
# case where sharing is not possible and a real copy is the only option.
for dat in "$SRC"/*.dat; do
    # A hard link cannot cross filesystems. The fallback COPIES ~2 GB per clone, which once put
    # ~2 TB on a shared filesystem unnoticed -- so say so every time it happens.
    cp -l "$dat" "$DST/" 2>/dev/null || {
        echo "clone-game: WARNING: cannot hard-link $dat into $DST (another filesystem?); COPYING it" >&2
        cp "$dat" "$DST/"
    }
done

echo "clone-game: $DST is $(du -sh "$DST" | cut -f1) apparent, of which \
$(du -sh --exclude='*.dat' "$DST" | cut -f1) is new disk"
