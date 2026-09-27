#!/bin/bash
#SBATCH --job-name=sfe-statecap
#SBATCH --partition=main
#SBATCH --cpus-per-task=16
#SBATCH --mem=32000
#SBATCH --time=24:00:00
#SBATCH --requeue
#SBATCH --open-mode=append
#SBATCH --output=%x.%A_%a.out
# NOT the halk nodes. Measured 2026-09-26, first 16-task array: all 6 tasks on hal nodes captured
# (472 ok), all 10 on halk nodes broke -- 4 died on "Xvfb :90 did not come up within 5s", 6 froze with
# the game spinning a core after 3 rendered frames, and a trial on halk0012 froze the same way. Both
# kinds advertise the same features (icelake, 64 cores), so a --constraint cannot separate them.
#SBATCH --exclude=halk[0001-0159]
#
# Regenerate the 33-channel state sidecars on Amarel CPU nodes: every replay in ~/sfe-replays,
# --no-video, gzipped, as a Slurm array. Each array task runs WORKERS capture workers side by side.
#
#   cd ~/sokubot-runs && sbatch --array=0-15 ~/sfe/ops/bwrap/amarel-statecap.slurm
#   WORKERS=2 LIMIT=1 sbatch --array=0-0 ...            # a quick trial: one replay per worker
#   TASKS=16 sbatch --array=6-15 ...                    # redo SOME tasks of a 16-task layout
#
# THE SLICE A WORKER GETS IS A PURE FUNCTION OF (task, worker, array size, WORKERS): shard
# k = task*WORKERS + w of N = tasks*WORKERS, over the sorted replay list (runner/collect.py strides
# it). So resubmitting the same array with the same WORKERS resumes every shard where it stopped
# (--resume skips what each shard's manifest already records as ok) -- which is also what a
# requeue after the time limit does. Changing the array size or WORKERS re-slices the corpus: the
# new shards start empty and captures already made are simply redone into them.
#
# What each worker gets, and why:
#   game     ~/sfe-work/games/k<shard>, a hardlink clone of ~/sfe-game (~22 MB). One game directory
#            cannot serve two workers: collect.py writes a per-job .ini into it (ops/bwrap/README.md).
#   prefix   node-local $TMPDIR (compute nodes' / is a RAM tmpfs): a fresh win32 prefix per job,
#            ~30 s of wineboot, gone with the job.
#   output   $STATECAP_OUT/w<k> on real disk (collect.py refuses a tmpfs output), default
#            /scratch/$USER/statecap. Named w<k> because that is the layout SokuBot's readers
#            walk (<root>/w*/<replay>): `--corpus /scratch/$USER/statecap` then feeds both
#            train_state_dynamics and the PPO bank as is. /scratch is purged after a while: build
#            the bank from it and keep the bank, not the sidecars.
#   cores    SFE_CPU_BLOCK=w, so the workers pin to disjoint blocks INSIDE this job's cpuset
#            (runner/throttle.py reads sched_getaffinity, not the node's core count).
set -uo pipefail
W=${WORKERS:-4}
CPUS=$(( ${SLURM_CPUS_PER_TASK:-16} / W ))
[ "$CPUS" -ge 1 ] || { echo "WORKERS=$W exceeds --cpus-per-task"; exit 2; }
# The layout's task count, NOT this submission's: resubmitting --array=6-15 of a 16-task layout
# has SLURM_ARRAY_TASK_COUNT=10, which would re-slice the corpus under the old shard names.
T=${TASKS:-${SLURM_ARRAY_TASK_COUNT:-1}}
t=${SLURM_ARRAY_TASK_ID:-0}
N=$(( T * W ))
OUTROOT=${STATECAP_OUT:-/scratch/$USER/statecap}
LOCAL=${TMPDIR:-/tmp}/sfe-statecap-${SLURM_JOB_ID:-$$}
# SFE_GAMES lets a trial run use its own clones: two jobs sharing a clone overwrite each other's
# per-job .ini, and a trial alongside a running array would corrupt the array's captures.
GAMES=${SFE_GAMES:-$HOME/sfe-work/games}
LIMIT_ARG=(); [ -n "${LIMIT:-}" ] && LIMIT_ARG=(--limit "$LIMIT")
mkdir -p "$OUTROOT/logs" "$LOCAL" "$GAMES"
trap 'rm -rf "$LOCAL"' EXIT

echo "== $(date -Is) task $t/$T restart ${SLURM_RESTART_COUNT:-0} on $(hostname): $W workers x $CPUS cpus, shards of $N"
echo "== cpus allowed: $(python3 -c 'import os; print(sorted(os.sched_getaffinity(0)))')"
pids=()
for w in $(seq 0 $((W - 1))); do
    k=$(( t * W + w ))
    g=$GAMES/k$k
    "$HOME/sfe/ops/bwrap/clone-game.sh" "$HOME/sfe-game" "$g" >/dev/null
    SFE_GAME=$g SFE_PREFIX=$LOCAL/prefix$w SFE_OUT=$OUTROOT/w$k \
    SFE_REPLAYS=$HOME/sfe-replays SFE_CPU_BLOCK=$w \
        "$HOME/sfe/ops/bwrap/sfe-bwrap" /app/docker/entrypoint.sh \
            --replay-dir /replays --out /out --shard "$k/$N" --no-video --gzip-csv --resume \
            --cpus "$CPUS" --timeout 900 --min-free-gb 20 "${LIMIT_ARG[@]}" \
        >> "$OUTROOT/logs/w$k.log" 2>&1 &
    pids+=($!)
done
rc=0
for p in "${pids[@]}"; do wait "$p" || rc=$?; done
echo "== $(date -Is) task $t done, last nonzero worker exit: $rc"
for w in $(seq 0 $((W - 1))); do
    k=$(( t * W + w ))
    m=$OUTROOT/w$k/manifest.jsonl
    [ -f "$m" ] && echo "   shard $k: $(grep -c '"ok"' "$m" 2>/dev/null || echo 0) manifest lines mentioning ok, $(wc -l < "$m") total"
done
exit "$rc"
