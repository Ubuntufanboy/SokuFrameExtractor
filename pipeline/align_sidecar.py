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
import gzip
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
SEARCH = 8            # +-frames; observed offsets are 0 or -1
# Frames that may be filled by repeating the nearest real row, total across
# both ends. Measured over 298 real mismatched pairs the shortfall is -35..+8
# with a median of 0 and a p90 of 2, on captures of ~11 000 frames; 60 is one
# second, well past that and far short of the 5955-frame outlier that is a
# genuinely broken pair.
MAX_PAD = 60


def sidecar(directory: Path) -> Path | None:
    """The capture's input CSV, compressed or not.

    Fresh state-only runs gzip the sidecar as they go -- at 8-11x that is what
    makes a full corpus fit on the box that captures it -- while the corpus
    captured before that change has plain files. Both have to be readable here
    or the alignment can only run on half the data it exists to join.
    """
    for name in ("inputs.csv", "inputs.csv.gz"):
        p = directory / name
        if p.exists():
            return p
    return None


def read_csv(path: Path) -> tuple[list[str], list[dict]]:
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", newline="") as fh:
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


VALID_COL = "label_valid"

# What a lean output keeps. The input columns are already in the corpus's own
# inputs.csv, so copying them alongside the video would duplicate 0.34 MB a
# replay -- 0.7 GB over the corpus -- to say something already on disk. These
# are exactly the columns sokubot.data.state.read_state reads.
STATE_COLS = tuple(f"p{p}_{c}" for p in (1, 2) for c in
                   ("x", "y", "dir", "action", "guarding", "wrongblock",
                    "crushed", "knockdown"))


def align_rows(old_n: int, new: list[dict], offset: int,
               max_pad: int) -> tuple[list[dict], int, int] | None:
    """New rows re-indexed so row i is the old capture's video frame i.

    Returns (rows, pad_head, pad_tail), or None if more than `max_pad` frames
    would have to be invented.

    THE ENDS DO NOT LINE UP, AND THAT IS NORMAL
    -------------------------------------------
    Capture arms and disarms a tick or so either side of the battle, and the
    results screen is presented a variable number of times for the same 354
    engine ticks. Measured over 298 mismatched pairs from the real corpus, the
    shortfall runs -35 to +8 frames with a median of 0 and a 90th percentile of
    2 -- out of about 11 000. Those frames are intro and results, not play.

    An earlier version rejected every one of these, including the 157 where
    the fresh capture actually covered the whole video and the offset merely
    happened to be negative. Rejecting a third of the corpus over half a second
    of post-match screen is the wrong trade.

    So the uncovered frames at each end are filled by repeating the nearest
    real row, and marked: `label_valid` is 0 on exactly those rows and 1
    everywhere else. Row i stays video frame i -- which is the invariant the
    whole file exists to provide -- and nothing invented is unmarked.
    """
    pad_head = max(0, -offset)
    pad_tail = max(0, (old_n + offset) - len(new))
    if pad_head + pad_tail > max_pad or not new:
        return None

    rows: list[dict] = []
    for i in range(old_n):
        j = i + offset
        valid = 0 <= j < len(new)
        src = new[min(max(j, 0), len(new) - 1)]
        rows.append({**src, VALID_COL: 1 if valid else 0})
    return rows, pad_head, pad_tail


def align_one(corpus_dir: Path, fresh_dir: Path, out_name: str,
              *, max_residual: float = MAX_RESIDUAL,
              min_separation: float = MIN_SEPARATION,
              max_pad: int = MAX_PAD,
              lean: bool = True,
              dry_run: bool = False) -> dict:
    """Align one capture pair. Returns a record; never raises for bad data."""
    rec: dict = {"replay_id": corpus_dir.name}
    old_path, new_path = sidecar(corpus_dir), sidecar(fresh_dir)
    if old_path is None or new_path is None:
        return {**rec, "status": "unreadable", "detail": "no inputs.csv[.gz]"}
    try:
        _, old = read_csv(old_path)
        header, new = read_csv(new_path)
    except (OSError, csv.Error, EOFError) as e:
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

    aligned = align_rows(len(old), new, offset, max_pad)
    if aligned is None:
        return {**rec, "status": "incomplete_coverage"}
    rows, pad_head, pad_tail = aligned
    rec.update(pad_head=pad_head, pad_tail=pad_tail)

    if not dry_run:
        # Written beside the video so `sokubot.data.state.read_state` consumes
        # it directly and row i is video frame i.
        #
        # Lean and gzipped by default because the arithmetic is stark: the full
        # sidecar is 0.71 MB a replay, 1.42 GB over the corpus, against 1.1 GB
        # free on the machine this was built for. State columns only is 0.73 GB
        # and gzipped about 0.1 GB. Nothing is lost -- the dropped columns are
        # the inputs, which the corpus capture beside this file already has.
        cols = ([*STATE_COLS, VALID_COL] if lean else [*header, VALID_COL])
        out = corpus_dir / out_name
        tmp = out.with_name(out.name + ".part")
        opener = gzip.open if out_name.endswith(".gz") else open
        with opener(tmp, "wt", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)
        tmp.rename(out)
    return {**rec, "status": "ok"}


def index_fresh(dirs: list[Path]) -> dict[str, Path]:
    """replay_id -> capture directory, across every fresh output tree."""
    out: dict[str, Path] = {}
    for root in dirs:
        for child in sorted(root.iterdir()) if root.is_dir() else []:
            if child.is_dir() and sidecar(child) is not None:
                out[child.name] = child
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--corpus", type=Path, required=True,
                    help="corpus root (a dir of worker dirs, or of captures)")
    ap.add_argument("--fresh", type=Path, nargs="+", required=True,
                    help="output trees from the --no-video re-capture")
    ap.add_argument("--out-name", default="state.csv.gz",
                    help="written into each corpus capture dir")
    ap.add_argument("--max-residual", type=float, default=MAX_RESIDUAL)
    ap.add_argument("--min-separation", type=float, default=MIN_SEPARATION)
    ap.add_argument("--max-pad", type=int, default=MAX_PAD,
                    help=f"frames that may be filled at the ends by repeating "
                         f"the nearest real row, marked label_valid=0 "
                         f"(default {MAX_PAD})")
    ap.add_argument("--full-columns", action="store_true",
                    help="keep the fresh capture's input columns too. They are "
                         "already in the corpus inputs.csv beside this file, "
                         "and keeping them triples the output.")
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
                        max_pad=args.max_pad,
                        lean=not args.full_columns,
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
    padded = [r for r in records
              if r.get("status") == "ok" and r.get("pad_head", 0)
              + r.get("pad_tail", 0) > 0]
    if padded:
        worst = max(r["pad_head"] + r["pad_tail"] for r in padded)
        total = sum(r["pad_head"] + r["pad_tail"] for r in padded)
        print(f"  padded at the ends:  {len(padded)} captures, "
              f"{total} frames total, worst {worst} "
              f"(marked {VALID_COL}=0)")
    return 0 if counts.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
