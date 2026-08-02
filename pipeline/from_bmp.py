"""Recover a modern capture (video.mp4 + inputs.csv) from a legacy BMP capture.

The pre-refactor extractor wrote one BMP per frame plus an inputs.csv carrying
an explicit ``frame_file`` column. Those captures are the only real gameplay
this project has recorded, so they are worth rescuing even though the CSV that
ships alongside them is partly corrupt.

WHAT IS AND IS NOT BROKEN IN THAT DATA
--------------------------------------
``frame_index`` is unreliable -- it jumps backwards, and in the FIFO-era files
it repeats a single value tens of thousands of times (see pipeline/validate.py
for the mechanism). ``frame_file`` is not: every row names its own BMP, and the
values are unique. So the row-to-frame pairing survives even where the frame
numbering does not.

Two defects remain and are handled here:

* **Rows are slightly out of temporal order** -- row 178 holds ``000179.bmp``
  while row 179 holds ``000178.bmp``. Sorting by the file's numeric index
  restores real time order, carrying each row's inputs with it.
* **Frames are missing.** manual_003 has 2,396 frames spread across indices
  0..9,735, so roughly three quarters were dropped. Treating those as
  consecutive would produce video that jumps. Instead the longest *contiguous*
  index run is selected, which is genuine unbroken 60 fps footage.
"""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
from pathlib import Path

FPS = 60
_NUM = re.compile(r"(\d+)")

MODERN_HEADER = [
    "frame", "game_frame", "p1_input", "p2_input",
    *[f"{p}_{b}" for p in ("p1", "p2")
      for b in ("up", "down", "left", "right",
                "a", "b", "c", "d", "change", "spell")],
]


def _index_of(frame_file: str) -> int:
    m = _NUM.search(frame_file)
    return int(m.group(1)) if m else -1


def load_ordered(capture: Path) -> list[dict]:
    """Rows in true temporal order, each still paired with its own frame."""
    rows = list(csv.DictReader((capture / "inputs.csv").open()))
    rows = [r for r in rows if r.get("frame_file")]
    rows.sort(key=lambda r: _index_of(r["frame_file"]))
    return rows


def contiguous_runs(rows: list[dict]) -> list[tuple[int, int]]:
    """(start, length) of maximal runs whose frame indices increment by one."""
    runs: list[tuple[int, int]] = []
    start = 0
    for i in range(1, len(rows) + 1):
        broken = (
            i == len(rows)
            or _index_of(rows[i]["frame_file"]) != _index_of(rows[i - 1]["frame_file"]) + 1
        )
        if broken:
            runs.append((start, i - start))
            start = i
    runs.sort(key=lambda r: -r[1])
    return runs


def activity(rows: list[dict]) -> int:
    return sum(
        1 for r in rows
        if int(r.get("p1_input") or 0) or int(r.get("p2_input") or 0)
    )


def convert(capture: Path, out_dir: Path, *, min_frames: int = 60,
            crf: int = 20, log=print) -> bool:
    rows = load_ordered(capture)
    if not rows:
        log(f"{capture.name}: no usable rows")
        return False

    runs = contiguous_runs(rows)
    start, length = runs[0]
    if length < min_frames:
        log(f"{capture.name}: longest contiguous run is only {length} frames")
        return False

    seg = rows[start:start + length]
    log(f"{capture.name}: {length} contiguous frames "
        f"({length / FPS:.1f}s), {activity(seg)} with input")

    out_dir.mkdir(parents=True, exist_ok=True)

    # --- video --------------------------------------------------------------
    # Built from a densely renumbered symlink sequence fed to the image2
    # demuxer, NOT from a concat list. The concat approach needs a duplicated
    # final entry to give the last frame a duration, and combined with
    # -vsync cfr that produced three extra frames -- which breaks the one
    # invariant this whole pipeline rests on, that CSV row i describes video
    # frame i. pipeline/validate.py caught it, and it is exactly the class of
    # silent misalignment that validator exists for.
    #
    # image2 with -frames:v len(seg) emits exactly one frame per input, so the
    # count is right by construction rather than by luck.
    frames_dir = capture / "frames"
    seq_dir = out_dir / ".seq"
    if seq_dir.exists():
        for f in seq_dir.iterdir():
            f.unlink()
    seq_dir.mkdir(parents=True, exist_ok=True)
    for i, r in enumerate(seg):
        (seq_dir / f"{i:06d}.bmp").symlink_to((frames_dir / r["frame_file"]).resolve())

    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-framerate", str(FPS),
        "-start_number", "0",
        "-i", str(seq_dir / "%06d.bmp"),
        "-frames:v", str(len(seg)),
        # NO vflip here. The raw-BGRA FIFO path does need one, because
        # glReadPixels returns rows bottom-up -- but the legacy BMP writer
        # already applied it, so these files are stored top-down. Flipping
        # again puts the health bars at the bottom and the text upside down.
        "-vf", "format=yuv420p",
        "-c:v", "libx264", "-preset", "veryfast", "-crf", str(crf),
        str(out_dir / "video.mp4"),
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    for f in seq_dir.iterdir():
        f.unlink()
    seq_dir.rmdir()
    if res.returncode != 0:
        log(f"ffmpeg failed: {res.stderr.strip()[:300]}")
        return False

    # --- inputs.csv in the modern schema ----------------------------------
    with (out_dir / "inputs.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(MODERN_HEADER)
        for i, r in enumerate(seg):
            w.writerow([
                i, _index_of(r["frame_file"]),
                r.get("p1_input", 0), r.get("p2_input", 0),
                *[r.get(c, 0) for c in MODERN_HEADER[4:]],
            ])

    log(f"wrote {out_dir}/video.mp4 + inputs.csv")
    return True


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Recover video.mp4 + inputs.csv from a legacy BMP capture.")
    ap.add_argument("capture", type=Path, help="dir with frames/ and inputs.csv")
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--min-frames", type=int, default=60)
    args = ap.parse_args(argv)

    if not (args.capture / "inputs.csv").exists():
        print(f"no inputs.csv in {args.capture}", file=sys.stderr)
        return 2
    return 0 if convert(args.capture, args.out, min_frames=args.min_frames) else 1


if __name__ == "__main__":
    raise SystemExit(main())
