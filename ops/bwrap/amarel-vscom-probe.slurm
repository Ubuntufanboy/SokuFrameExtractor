#!/bin/bash
#SBATCH --job-name=sfe-vscom-probe
#SBATCH --partition=main
#SBATCH --cpus-per-task=24
#SBATCH --mem=24000
#SBATCH --time=01:00:00
#SBATCH --output=%x.%j.out
#SBATCH --exclude=halk[0001-0159]
#
# Phase 0 of the vs-COM environment: prove the menu-free vs-COM start before anything is built on
# it. One match per run, P1 driven by the existing scripted self-play policy (no agent channel yet),
# P2 left to the game's AI. Runs COM level 0 and level 3 with the same three seeds, so the difficulty
# field (setup+0, written via config+0x64 -- a hypothesis from the binary) is tested by its effect
# on play, not by reading back what was written.
#
#   cd ~/sokubot-runs && sbatch ~/sfe/ops/bwrap/amarel-vscom-probe.slurm
#
# Needs: ~/sfe synced, the new DLL at ~/sfe-vscom-dll/SokuFrameExtractor.dll, ~/sfe-game (staged by
# stage-game.sh), and ~/sfe-replays (one replay is handed to the runner; vs COM never reads it).
#
# What to read in the log (per run, from the module log beside the DLL):
#   "vscom: start ..."   deck sizes (0 cards = prepareBattle did not build a deck) and the level
#   "vscom: frame ..."   LIVE character ids, matchState changes, scores, and whether P2's input
#                        varies (it must, or the AI is not driving it)
#   "Perf: ..."          fps, game ticks/s, battle_frame/s
#   "vscom: scene ..."   which scenes follow a finished match (what a restart loop must hook)
set -uo pipefail
OUT=${PROBE_OUT:-/scratch/$USER/vscom-probe}
BASE=$HOME/sfe-vscom-work
LOCAL=${TMPDIR:-/tmp}/sfe-vscom-${SLURM_JOB_ID:-$$}
mkdir -p "$OUT" "$BASE" "$LOCAL"
trap 'rm -rf "$LOCAL"' EXIT
REPS=$LOCAL/replays
mkdir -p "$REPS"
cp "$(ls ~/sfe-replays/*.rep | head -1)" "$REPS/"

echo "== $(date -Is) on $(hostname); cpus $(python3 -c 'import os; print(sorted(os.sched_getaffinity(0)))')"
echo "== sfe deployed: $(cat ~/sfe/DEPLOYED 2>/dev/null || echo unknown) | dll $(sha256sum ~/sfe-vscom-dll/SokuFrameExtractor.dll | cut -c1-16)"
pids=(); w=0
for level in 0 3; do
  for seed in 1 2 3; do
    tag=L${level}s${seed}
    g=$BASE/game-$tag
    rm -rf "$g"
    "$HOME/sfe/ops/bwrap/clone-game.sh" "$HOME/sfe-game" "$g" >/dev/null
    # A clone COPIES everything but the .dat archives, so replacing the DLL here
    # cannot touch ~/sfe-game.
    cp ~/sfe-vscom-dll/SokuFrameExtractor.dll "$g/modules/SokuFrameExtractor/SokuFrameExtractor.dll"
    rm -rf "$OUT/$tag"
    SFE_GAME=$g SFE_PREFIX=$LOCAL/prefix$w SFE_OUT=$OUT/$tag SFE_REPLAYS=$REPS SFE_CPU_BLOCK=$w \
    SFE_VSCOM=1 SFE_COM_LEVEL=$level SFE_SELFPLAY=1 SFE_SEED=$seed SFE_WIRE_KEYMGR=1 \
    SFE_MAX_FRAMES=60000 \
        "$HOME/sfe/ops/bwrap/sfe-bwrap" /app/docker/entrypoint.sh \
            --replay-dir /replays --out /out --shard 0/1 --no-video --gzip-csv \
            --cpus 4 --timeout 900 --min-free-gb 5 --limit 1 \
        > "$OUT/$tag.runner.log" 2>&1 &
    pids+=($!); w=$((w + 1))
  done
done
for p in "${pids[@]}"; do wait "$p"; done
echo "== $(date -Is) all runs finished"
for level in 0 3; do
  for seed in 1 2 3; do
    tag=L${level}s${seed}
    echo "################ $tag"
    log=$BASE/game-$tag/modules/SokuFrameExtractor/SokuFrameExtractor.log
    grep -E "vscom|Perf|setBattleMode|pollHook|wireKeyManagers|selfplay: behaviour|ERROR|fail" "$log" 2>/dev/null | head -80
    tail -3 "$OUT/$tag.runner.log"
  done
done
