#!/usr/bin/env bash
# The counterfactual pipeline, end to end, in one command.
#
#   ops/cf_check.sh <replay.rep> [CF_FRAME] [CF_LEN] [CF_PLAYER] [CF_DIR]
#
# Runs three captures of ONE replay in BATTLE_SUBMODE_REPLAY:
#
#   A  control          no SFE_CF_* at all
#   B  control repeat   identical to A -- this is the determinism gate
#   F  forced           the intervention, direction bits only
#
# Then compares A vs B and A vs F.
#
# WHY THE GATE COMES FIRST
# ------------------------
# A pair only means something if the two runs are identical BEFORE the window.
# Self-play cannot give that: its mode schedule advances per hook call while the
# window is keyed to the battle frame, and the battle frame freezes during
# hitstop, so the clocks slip (measured: 6 of 671 frames disagreeing before the
# window). A replay has no policy at all -- both players' inputs come from the
# recorded stream -- so identity is a property of the vehicle. A vs B tests
# exactly that, and if it does not print zero, nothing else here is a
# counterfactual.
#
# Needs a host where capture actually works. See
# memory/soku-capture-on-this-laptop-is-broken.md before running it locally.
set -euo pipefail

REP="${1:?usage: cf_check.sh <replay.rep> [frame] [len] [player] [dir]}"
FRAME="${2:-600}"
LEN="${3:-30}"
PLAYER="${4:-2}"
DIR="${5:-1}"

ROOT="${CF_ROOT:-$HOME/cf-check}"
PREFIX="${SFE_PREFIX:-$HOME/.wine-soku}"
SOKUBOT="${SOKUBOT:-$HOME/K0NTR0L-2/SokuBot}"
MAXF="${SFE_MAX_FRAMES:-3000}"

mkdir -p "$ROOT/reps"
cp -f "$REP" "$ROOT/reps/cf.rep"

run() {                       # run <outdir>  [extra env already exported]
    rm -rf "$ROOT/$1"
    SFE_MAX_FRAMES="$MAXF" python3 -m runner.collect \
        --prefix "$PREFIX" --replay-dir "$ROOT/reps" --out "$ROOT/$1" \
        --no-video --gzip-csv --cpus "${CF_CPUS:-2}" --timeout 600 \
        --min-free-gb 1 2>&1 | tail -3
}

echo "=== A: control ==="
( unset SFE_CF_FRAME SFE_CF_LEN SFE_CF_PLAYER SFE_CF_DIR; run runA )
echo "=== B: control repeat (determinism gate) ==="
( unset SFE_CF_FRAME SFE_CF_LEN SFE_CF_PLAYER SFE_CF_DIR; run runB )
echo "=== F: forced, player $PLAYER, frames $FRAME..$((FRAME+LEN-1)) ==="
( export SFE_CF_FRAME="$FRAME" SFE_CF_LEN="$LEN" \
         SFE_CF_PLAYER="$PLAYER" SFE_CF_DIR="$DIR"; run runF )

ID=$(basename "$(find "$ROOT/runA" -maxdepth 1 -mindepth 1 -type d ! -name '.*' | head -1)")
[ -n "$ID" ] || { echo "no capture produced; nothing to compare"; exit 1; }

cd "$SOKUBOT"
echo
echo "################ DETERMINISM GATE (A vs B) ################"
python3 -m scripts.cf_pair --a "$ROOT/runA/$ID" --b "$ROOT/runB/$ID" || {
    echo
    echo "GATE FAILED -- the vehicle is not deterministic, so the pair below"
    echo "is not a counterfactual. Do not read the effect."
    exit 1
}
echo
echo "################ COUNTERFACTUAL (A vs F) ################"
python3 -m scripts.cf_pair --a "$ROOT/runA/$ID" --b "$ROOT/runF/$ID" \
    --frame "$FRAME" --len "$LEN" --player "$PLAYER"
