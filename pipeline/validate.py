"""Integrity gate for a captured replay.

This module exists because its absence let visibly broken data through. In the
pre-refactor output under ``Soku/soku_extract/``:

    replay_002/inputs.csv -- 37,733 rows, 9,492 distinct local_frame values,
                             frame 9718 repeated 28,012 times, not monotonic
    replay_003/inputs.csv -- same shape, frame 8622 repeated 31,789 times

Roughly three quarters of each file is the same game tick recorded over and
over: the capture kept running while the state machine was not advancing the
frame counter, so menu and result-screen frames were written carrying a stale
gameplay label. Nothing in the pipeline noticed, and that data would have gone
straight into training.

The checks below are ordered cheapest-first so a broken capture fails before we
spend time decoding video.

Run standalone against any capture tree:

    python3 -m pipeline.validate out/
    python3 -m pipeline.validate Soku/soku_extract/ --legacy
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

# A capture legitimately repeats one frame index at the capture/extract
# boundary: the swap-buffers hook arms one frame before the FSM stages its
# first index, so frame 0 can appear twice. Anything beyond a rounding error's
# worth of repeats means capture ran outside the extracting state.
DUPLICATE_RATIO_ERROR = 0.01  # 1%

# Below this, a capture is too short to be a real match (Soku rounds run ~60s+).
MIN_USABLE_FRAMES = 600


class Severity(str, Enum):
    OK = "ok"
    WARN = "warn"
    ERROR = "error"


@dataclass
class Finding:
    check: str
    severity: Severity
    detail: str

    def __str__(self) -> str:
        return f"[{self.severity.value.upper():5}] {self.check}: {self.detail}"


@dataclass
class Report:
    target: Path
    findings: list[Finding] = field(default_factory=list)
    stats: dict = field(default_factory=dict)

    def add(self, check: str, severity: Severity, detail: str) -> None:
        self.findings.append(Finding(check, severity, detail))

    @property
    def ok(self) -> bool:
        return not any(f.severity is Severity.ERROR for f in self.findings)

    @property
    def errors(self) -> list[Finding]:
        return [f for f in self.findings if f.severity is Severity.ERROR]

    def to_json(self) -> dict:
        return {
            "target": str(self.target),
            "ok": self.ok,
            "stats": self.stats,
            "findings": [
                {"check": f.check, "severity": f.severity.value, "detail": f.detail}
                for f in self.findings
            ],
        }


# ---------------------------------------------------------------------------
# CSV checks
# ---------------------------------------------------------------------------

# Which column carries the *engine tick* a row came from. This is the one that
# reveals capture running outside the extracting state, because it stalls while
# rows keep being written.
#
# Three generations of schema exist and all three are accepted, so this can be
# pointed at historical captures for regression testing:
#   game_frame   current
#   local_frame  pre-refactor (per-replay, recomputed against a shared offset)
#   frame_index  BMP era
_TICK_COLS = ("game_frame", "local_frame", "frame_index")

# The current schema additionally carries `frame`: a dense row counter written
# by the encoder thread. It is what video frame N maps to, and it is checked
# separately -- a gap in it means rows were lost between the ring and the file.
_ROW_COL = "frame"

# -------------------------------------------------------------------------
# The CSV carries a free error-detecting code, and nothing was checking it
# -------------------------------------------------------------------------
# Every row stores each player's inputs twice: once as a 10-bit mask
# (`p1_input`) and once expanded into ten boolean columns. Both are written
# from the same uint16 in the same encoder-thread statement, so on honest
# hardware they cannot disagree.
#
# That redundancy is a parity check over exactly the field that matters most.
# A flipped bit in the labels is the worst corruption this project can ship:
# it is invisible to a human, survives every structural check, and teaches a
# world model that an action it never saw produced an outcome it never caused.
# Video corruption is comparatively benign -- a few wrong pixels in one frame
# of a 10,000-frame episode.
#
# So on hardware with known bit flips, this is the check that earns its keep.
# It catches any single-bit error in either representation, plus any multi-bit
# error that does not happen to corrupt both copies identically.
_BUTTON_ORDER = ("up", "down", "left", "right",
                 "a", "b", "c", "d", "change", "spell")
_INPUT_ALL_MASK = 0x3FF


def _mask_from_bools(row: dict, who: str) -> int:
    """Rebuild the input mask from the expanded boolean columns."""
    m = 0
    for i, name in enumerate(_BUTTON_ORDER):
        if int(row.get(f"{who}_{name}", 0) or 0):
            m |= 1 << i
    return m


def _tick_column(fieldnames: list[str]) -> str | None:
    for c in _TICK_COLS:
        if c in fieldnames:
            return c
    return None


def check_csv(csv_path: Path, report: Report) -> list[int] | None:
    """Validate the per-frame input CSV. Returns the frame index column."""
    if not csv_path.exists():
        report.add("csv.exists", Severity.ERROR, f"missing {csv_path.name}")
        return None

    with open(csv_path, newline="") as fh:
        reader = csv.DictReader(fh)
        if not reader.fieldnames:
            report.add("csv.header", Severity.ERROR, "empty file")
            return None

        col = _tick_column(list(reader.fieldnames))
        if col is None:
            report.add(
                "csv.header",
                Severity.ERROR,
                f"no engine-tick column; expected one of {_TICK_COLS}, "
                f"got {reader.fieldnames[:4]}",
            )
            return None
        has_row_col = _ROW_COL in reader.fieldnames

        frames: list[int] = []
        rows: list[int] = []
        p1_any = p2_any = False
        bad_rows = 0
        mask_mismatch = 0
        mask_range = 0
        first_bad: str | None = None
        have_bools = all(f"p1_{b}" in reader.fieldnames for b in _BUTTON_ORDER)
        for row in reader:
            try:
                frames.append(int(row[col]))
                if has_row_col:
                    rows.append(int(row[_ROW_COL]))
                for who in ("p1", "p2"):
                    m = int(row.get(f"{who}_input", 0) or 0)
                    if who == "p1":
                        p1_any |= m != 0
                    else:
                        p2_any |= m != 0
                    if m & ~_INPUT_ALL_MASK:
                        mask_range += 1
                        if first_bad is None:
                            first_bad = f"row {len(frames)}: {who}_input={m}"
                    if have_bools and _mask_from_bools(row, who) != m:
                        mask_mismatch += 1
                        if first_bad is None:
                            first_bad = (f"row {len(frames)}: {who}_input={m} but "
                                         f"columns say {_mask_from_bools(row, who)}")
            except (ValueError, TypeError):
                bad_rows += 1

    n = len(frames)
    report.stats["csv_rows"] = n
    report.stats["frame_column"] = col

    if bad_rows:
        report.add("csv.parse", Severity.ERROR, f"{bad_rows} unparseable row(s)")
    if mask_range:
        report.add(
            "csv.mask_range", Severity.ERROR,
            f"{mask_range} input value(s) with bits outside 0x{_INPUT_ALL_MASK:03X} "
            f"({first_bad}). The game cannot produce these, so the value was "
            f"corrupted after it was read.",
        )
    if mask_mismatch:
        report.add(
            "csv.mask_bits", Severity.ERROR,
            f"{mask_mismatch} row(s) where the input bitmask disagrees with its "
            f"own boolean columns ({first_bad}). Both are written from the same "
            f"value, so any disagreement is corruption.",
        )
    report.stats["mask_mismatches"] = mask_mismatch
    if n == 0:
        report.add("csv.rows", Severity.ERROR, "no data rows")
        return None

    # --- monotonicity: the strongest signal that capture ran out of state ---
    decreases = sum(1 for a, b in zip(frames, frames[1:]) if b < a)
    report.stats["decreases"] = decreases
    if decreases:
        report.add(
            "csv.monotonic",
            Severity.ERROR,
            f"{col} decreases {decreases} time(s) -- frames are out of order, "
            "so rows cannot be trusted to describe the video frames they sit on",
        )

    # --- duplicates ---
    distinct = len(set(frames))
    dupes = n - distinct
    ratio = dupes / n
    report.stats["distinct_frames"] = distinct
    report.stats["duplicate_rows"] = dupes
    report.stats["duplicate_ratio"] = round(ratio, 4)

    if ratio > DUPLICATE_RATIO_ERROR:
        worst_val, worst_n = _most_common(frames)
        report.add(
            "csv.duplicates",
            Severity.ERROR,
            f"{dupes}/{n} rows ({ratio:.1%}) repeat an earlier {col}; "
            f"worst is frame {worst_val} x{worst_n}. Capture ran while the FSM "
            "was not advancing -- these frames are mislabelled menu/result "
            "screens, not gameplay",
        )
    elif dupes:
        report.add(
            "csv.duplicates",
            Severity.WARN,
            f"{dupes}/{n} duplicate row(s) ({ratio:.2%}) -- consistent with the "
            "known capture/extract boundary artefact",
        )

    if frames[0] != 0:
        report.add("csv.start", Severity.WARN, f"{col} starts at {frames[0]}, not 0")

    # --- the row counter must be dense: 0,1,2,...,n-1 with no gaps ---------
    # This is the column video frame N maps to, so a gap means the CSV and the
    # video have silently diverged even though both files look healthy.
    if rows:
        if rows != list(range(n)):
            first_bad = next(
                (i for i, v in enumerate(rows) if v != i), len(rows)
            )
            report.add(
                "csv.rowcounter",
                Severity.ERROR,
                f"`{_ROW_COL}` is not dense 0..{n-1}; first mismatch at row "
                f"{first_bad} (value {rows[first_bad] if first_bad < n else 'n/a'})",
            )
    elif col == "game_frame":
        report.add(
            "csv.rowcounter", Severity.WARN, f"no `{_ROW_COL}` column present"
        )

    # --- inputs actually present ---
    if not (p1_any or p2_any):
        report.add(
            "csv.inputs",
            Severity.ERROR,
            "every input is zero -- the BattleManager read failed, so this "
            "capture has pixels but no actions",
        )
    elif not p1_any or not p2_any:
        who = "p1" if not p1_any else "p2"
        report.add("csv.inputs", Severity.WARN, f"{who} never pressed anything")

    if n < MIN_USABLE_FRAMES:
        report.add(
            "csv.length",
            Severity.WARN,
            f"only {n} frames (<{MIN_USABLE_FRAMES}); too short to be a full match",
        )

    return frames


def _most_common(values: list[int]) -> tuple[int, int]:
    from collections import Counter

    (val, count), = Counter(values).most_common(1)
    return val, count


# ---------------------------------------------------------------------------
# Video checks
# ---------------------------------------------------------------------------


def ffprobe_frame_count(video: Path) -> int | None:
    """Count coded frames. Returns None if ffprobe is unavailable or fails.

    Counts *packets*, not decoded frames, and deliberately does not trust the
    container header. Measured on a real 18 MB / 6055-frame capture, with the
    file truncated to a fraction of its bytes:

        bytes kept   count_frames   count_packets   header nb_frames
           100%          6055           6055             6055
            90%          5302           5303             6055
            60%          3523           3524             6055
            30%          1738           1739             6055

    So the header is useless -- it reports the full count for a file that is
    70% missing, which is exactly the failure this check exists to catch.

    Packets track truncation as faithfully as full decoding does, differing by
    one only on a damaged file (a trailing partial packet that will not
    decode), where we are going to fail anyway. On an intact file the two agree
    exactly, which is the case the 1:1 invariant is asserted against.

    Decoding cost 14.30 s for that file; counting packets cost 0.21 s. That
    ran after every capture on every worker, so it was ~14 s of dead time per
    replay during which the worker captured nothing.
    """
    try:
        res = subprocess.run(
            [
                "ffprobe", "-v", "error",
                "-select_streams", "v:0",
                "-count_packets",
                "-show_entries", "stream=nb_read_packets",
                "-of", "default=nokey=1:noprint_wrappers=1",
                str(video),
            ],
            capture_output=True, text=True, timeout=600,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    out = res.stdout.strip()
    return int(out) if out.isdigit() else None


def check_video(video: Path, csv_rows: int, report: Report) -> None:
    if not video.exists():
        report.add("video.exists", Severity.ERROR, f"missing {video.name}")
        return

    if video.stat().st_size == 0:
        report.add("video.size", Severity.ERROR, "video is 0 bytes")
        return

    n = ffprobe_frame_count(video)
    if n is None:
        report.add("video.decode", Severity.ERROR, "ffprobe could not decode the video")
        return

    report.stats["video_frames"] = n

    # The encoder writes one CSV row and one video frame from the same ring
    # slot, so these are 1:1 by construction. Asserting it catches a truncated
    # or partially-flushed mp4, which construction does NOT guarantee.
    if n != csv_rows:
        report.add(
            "video.sync",
            Severity.ERROR,
            f"{n} video frames vs {csv_rows} CSV rows (delta {n - csv_rows}); "
            "frame/input alignment is broken",
        )


# ---------------------------------------------------------------------------
# Top level
# ---------------------------------------------------------------------------


def validate_capture(directory: Path, *, require_video: bool = True) -> Report:
    """Validate one capture directory."""
    report = Report(target=directory)

    csv_path = directory / "inputs.csv"
    frames = check_csv(csv_path, report)

    video = directory / "video.mp4"
    if frames is not None:
        if video.exists():
            check_video(video, len(frames), report)
        elif require_video:
            report.add("video.exists", Severity.ERROR, "missing video.mp4")
        else:
            report.add("video.exists", Severity.WARN, "no video.mp4 (CSV-only check)")

    return report


def iter_captures(root: Path):
    """Yield capture directories under `root` (or `root` itself if it is one)."""
    if (root / "inputs.csv").exists():
        yield root
        return
    for child in sorted(root.iterdir()):
        if child.is_dir() and (child / "inputs.csv").exists():
            yield child


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Validate captured replay data (CSV/video integrity).",
    )
    ap.add_argument("root", type=Path, help="capture dir, or a dir of capture dirs")
    ap.add_argument(
        "--legacy", action="store_true",
        help="don't require video.mp4 (for pre-refactor BMP-era output)",
    )
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--quiet", "-q", action="store_true", help="only show failures")
    args = ap.parse_args(argv)

    if not args.root.exists():
        print(f"no such path: {args.root}", file=sys.stderr)
        return 2

    reports = [
        validate_capture(d, require_video=not args.legacy)
        for d in iter_captures(args.root)
    ]

    if not reports:
        print(f"no captures found under {args.root}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps([r.to_json() for r in reports], indent=2))
    else:
        for r in reports:
            if args.quiet and r.ok:
                continue
            verdict = "PASS" if r.ok else "FAIL"
            print(f"{verdict}  {r.target.name}  ({r.stats.get('csv_rows', 0)} rows)")
            for f in r.findings:
                if f.severity is not Severity.OK:
                    print(f"        {f}")

    n_ok = sum(1 for r in reports if r.ok)
    print(f"\n{n_ok}/{len(reports)} capture(s) passed", file=sys.stderr)
    return 0 if n_ok == len(reports) else 1


if __name__ == "__main__":
    raise SystemExit(main())
