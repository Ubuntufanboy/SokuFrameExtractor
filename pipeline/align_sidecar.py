"""Pair a freshly captured sidecar with the corpus video it belongs to.

    python3 -m pipeline.align_sidecar --corpus ~/corpus \
        --fresh ~/state-out0 ~/state-out1 --out-name state.csv

WHAT THIS IS FOR
----------------
The corpus has 2003 videos and sidecars without game state. Re-running each
replay with `--no-video` produces a new sidecar that has the state columns and
costs 1.3 MB instead of 34 MB. The pixels are unchanged, so the cheap path is
to keep the old video and attach the new labels.

The catch is that the two captures do not start on the same engine tick.
Capture arms within a tick or so of the battle beginning, not at a fixed
point, so runs of one replay differ in length -- 6055 / 6058 / 6061 were
measured for replay 5314129 -- and a fleet capture came out offset by exactly
one from a fresh one. Zipping them naively shifts every label by a frame on
some replays and not others, which is the kind of error that does not announce
itself: the model simply learns a slightly wrong association.

HOW THE OFFSET IS FOUND
-----------------------
Both files carry the twenty button columns, and those come from the replay
stream rather than from the renderer, so they are the same sequence in both.
Cross-correlating them recovers the shift outright: on the measured pair the
mismatch is 15.6% at the wrong offset and 0.132% at the right one, which is
not a close call. `battle_frame` would settle it directly, but only new
captures have that column -- the corpus predates it -- so the inputs are the
key that works for the data that actually exists.

WHY IT REFUSES RATHER THAN GUESSES
----------------------------------
Two guards, both of which reject instead of picking a best-effort answer:

  * the residual at the chosen offset must be small in absolute terms, and
  * it must be decisively better than the runner-up.

A replay whose inputs genuinely match at two different shifts is one where the
alignment is not determined by the evidence, and a mislabelled capture is
worse than a missing one -- it is 11 000 frames of confidently wrong
supervision that nothing downstream can detect.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

BUTTONS = ("up", "down", "left", "right", "a", "b", "c", "d", "change", "spell")
INPUT_COLS = tuple(f"p{p}_{b}" for p in (1, 2) for b in BUTTONS)

# Measured residuals at the true offset: 0.066% between two runs on one
# machine, 0.132% between a fleet capture and a fresh one. At a wrong offset
# the same pair reads 15.6%. One percent sits an order of magnitude above the
# former and an order below the latter.
MAX_RESIDUAL = 0.01
# The runner-up must be this many times worse. Measured separation on the same
# pair was 0.132% against 15.5%, i.e. 117x, so 5x is not a tight requirement --
# it exists to catch replays with long idle stretches where several shifts fit
# equally well and the evidence does not actually pick one.
MIN_SEPARATION = 5.0
SEARCH = 8            # +-frames; observed offsets are 0 or 1


def read_csv(path: Path) -> tuple[list[str], list[dict]]:
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        return list(reader.fieldnames or []), list(reader)


def _keys(rows: list[dict]) -> list[tuple]:
    return [tuple(r.get(c) for c in INPUT_COLS) for r in rows]


def find_offset(old: list[dict], new: list[dict], *,
                search: int = SEARCH) -> tuple[int, float, float, int]:
    """Shift s minimising disagreement, where old[i] should equal new[i + s].

    Returns (offset, residual, runner_up_residual, overlap). Residuals are
    fractions of the compared rows; `runner_up` is the best of the other
    shifts, which is what says whether the answer was actually determined.
    """
    ko, kn = _keys(old), _keys(new)
    scored: list[tuple[float, int, int]] = []
    for s in range(-search, search + 1):
        lo, hi = max(0, -s), min(len(ko), len(kn) - s)
        if hi - lo < 100:            # too little overlap to mean anything
            continue
        bad = sum(1 for i in range(lo, hi) if ko[i] != kn[i + s])
        scored.append((bad / (hi - lo), s, hi - lo))
    if not scored:
        return 0, 1.0, 1.0, 0
    scored.sort()
    best, runner = scored[0], (scored[1] if len(scored) > 1 else (1.0, 0, 0))
    return best[1], best[0], runner[0], best[2]


def align_rows(old_n: int, new: list[dict], offset: int) -> list[dict] | None:
    """New-capture rows re-indexed so row i is the old capture's video frame i.

    None when the new capture does not cover the whole video: a partial label
    track would silently end early, and a caller cannot tell that from a short
    replay.
    """
    if offset < 0 or old_n + offset > len(new):
        lo, hi = offset, old_n + offset
        if lo < 0 or hi > len(new):
            return None
    return [new[i + offset] for i in range(old_n)]


def align_one(corpus_dir: Path, fresh_dir: Path, out_name: str,
              *, max_residual: float = MAX_RESIDUAL,
              min_separation: float = MIN_SEPARATION,
              dry_run: bool = False) -> dict:
    """Align one capture pair. Returns a record; never raises for bad data."""
    rec: dict = {"replay_id": corpus_dir.name}
    try:
        _, old = read_csv(corpus_dir / "inputs.csv")
        header, new = read_csv(fresh_dir / "inputs.csv")
    except (OSError, csv.Error) as e:
        return {**rec, "status": "unreadable", "detail": str(e)}

    missing = [c for c in INPUT_COLS if c not in header]
    if missing:
        return {**rec, "status": "no_input_columns", "detail": str(missing[:3])}

    offset, residual, runner, overlap = find_offset(old, new)
    rec.update(offset=offset, residual=round(residual, 5),
               runner_up=round(runner, 5), overlap=overlap,
               old_rows=len(old), new_rows=len(new))

    if overlap == 0:
        return {**rec, "status": "no_overlap"}
    if residual > max_residual:
        return {**rec, "status": "residual_too_high"}
    if residual > 0 and runner / residual < min_separation:
        return {**rec, "status": "ambiguous"}
    if residual == 0 and runner == 0:
        return {**rec, "status": "ambiguous"}

    rows = align_rows(len(old), new, offset)
    if rows is None:
        return {**rec, "status": "incomplete_coverage"}

    if not dry_run:
        # Written beside the video, with the same header as the fresh capture,
        # so `sokubot.data.state.read_state` consumes it unchanged and row i is
        # video frame i.
        out = corpus_dir / out_name
        tmp = out.with_suffix(".part")
        with tmp.open("w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=header)
            w.writeheader()
            w.writerows(rows)
        tmp.rename(out)
    return {**rec, "status": "ok"}


def index_fresh(dirs: list[Path]) -> dict[str, Path]:
    """replay_id -> capture directory, across every fresh output tree."""
    out: dict[str, Path] = {}
    for root in dirs:
        for child in sorted(root.iterdir()) if root.is_dir() else []:
            if (child / "inputs.csv").exists():
                out[child.name] = child
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--corpus", type=Path, required=True,
                    help="corpus root (a dir of worker dirs, or of captures)")
    ap.add_argument("--fresh", type=Path, nargs="+", required=True,
                    help="output trees from the --no-video re-capture")
    ap.add_argument("--out-name", default="state.csv",
                    help="written into each corpus capture dir")
    ap.add_argument("--max-residual", type=float, default=MAX_RESIDUAL)
    ap.add_argument("--min-separation", type=float, default=MIN_SEPARATION)
    ap.add_argument("--report", type=Path, default=None)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    fresh = index_fresh(args.fresh)
    if not fresh:
        print(f"no captures under {args.fresh}", file=sys.stderr)
        return 2

    targets = [d for d in sorted(args.corpus.rglob("inputs.csv"))]
    counts: dict[str, int] = {}
    records = []
    for csv_path in targets:
        d = csv_path.parent
        f = fresh.get(d.name)
        if f is None:
            counts["no_fresh_capture"] = counts.get("no_fresh_capture", 0) + 1
            continue
        rec = align_one(d, f, args.out_name,
                        max_residual=args.max_residual,
                        min_separation=args.min_separation,
                        dry_run=args.dry_run)
        records.append(rec)
        counts[rec["status"]] = counts.get(rec["status"], 0) + 1

    if args.report:
        args.report.write_text("\n".join(json.dumps(r) for r in records) + "\n")

    print(f"{len(targets)} corpus captures, {len(fresh)} fresh sidecars")
    for k in sorted(counts, key=lambda k: -counts[k]):
        print(f"  {k:<22} {counts[k]}")
    offsets: dict[int, int] = {}
    for r in records:
        if r.get("status") == "ok":
            offsets[r["offset"]] = offsets.get(r["offset"], 0) + 1
    if offsets:
        print("  offsets seen:        "
              + ", ".join(f"{k:+d}: {v}" for k, v in sorted(offsets.items())))
    return 0 if counts.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
