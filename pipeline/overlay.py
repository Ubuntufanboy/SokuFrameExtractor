"""Render a controller overlay onto a captured replay.

This is a QA instrument first and a presentation asset second. Every automated
check in validate.py is structural -- row counts line up, indices increase, the
video decodes -- and none of them can tell you whether the inputs recorded on
frame N are the inputs that produced the action visible on frame N. A human
watching a button light up at the same instant a character swings can tell
instantly, and so can anyone reviewing the project on GitHub.

Implementation notes:

* Panels are drawn with pure stdlib into a raw BGRA stream piped to FFmpeg,
  which composites them over the video. No Pillow, no numpy -- the runner image
  is deliberately dependency-free, and the drawing is a few filled rectangles.
* Only the requested clip range is rendered, so cost scales with clip length
  rather than replay length.
"""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

FPS = 60

# --- panel geometry (per player) -------------------------------------------
PANEL_W, PANEL_H = 132, 76
PAD = 8
BTN = 20          # button square size
BTN_GAP = 4
DPAD = 18         # d-pad cell size

# BGRA colours
C_BG        = (24, 20, 18, 190)     # translucent charcoal
C_BORDER    = (90, 80, 76, 220)
C_OFF       = (58, 52, 50, 235)
C_ON_BTN    = (90, 200, 255, 255)   # amber-ish in BGRA -> reads warm
C_ON_DIR    = (120, 235, 140, 255)  # green
C_TEXT      = (210, 205, 200, 255)

BUTTONS = ["a", "b", "c", "d", "change", "spell"]
BUTTON_LABEL = {"a": "A", "b": "B", "c": "C", "d": "D", "change": "CH", "spell": "SP"}


@dataclass
class Row:
    dirs: dict            # up/down/left/right -> bool
    btns: dict            # a..spell -> bool


def read_rows(csv_path: Path, player: str) -> list[Row]:
    rows: list[Row] = []
    with open(csv_path, newline="") as fh:
        for r in csv.DictReader(fh):
            rows.append(
                Row(
                    dirs={d: r.get(f"{player}_{d}") == "1"
                          for d in ("up", "down", "left", "right")},
                    btns={b: r.get(f"{player}_{b}") == "1" for b in BUTTONS},
                )
            )
    return rows


class Canvas:
    """Minimal BGRA raster. Rectangles and a 3x5 bitmap font are all we need."""

    def __init__(self, w: int, h: int):
        self.w, self.h = w, h
        self.buf = bytearray(w * h * 4)   # transparent

    def rect(self, x: int, y: int, w: int, h: int, colour) -> None:
        b, g, r, a = colour
        px = bytes((b, g, r, a))
        for yy in range(max(0, y), min(self.h, y + h)):
            base = (yy * self.w + max(0, x)) * 4
            n = min(self.w, x + w) - max(0, x)
            if n > 0:
                self.buf[base:base + n * 4] = px * n

    def frame(self, x: int, y: int, w: int, h: int, colour, t: int = 1) -> None:
        self.rect(x, y, w, t, colour)
        self.rect(x, y + h - t, w, t, colour)
        self.rect(x, y, t, h, colour)
        self.rect(x + w - t, y, t, h, colour)

    def text(self, x: int, y: int, s: str, colour, scale: int = 1) -> None:
        for i, ch in enumerate(s.upper()):
            glyph = FONT.get(ch)
            if not glyph:
                continue
            for gy, line in enumerate(glyph):
                for gx, on in enumerate(line):
                    if on == "1":
                        self.rect(x + (i * 4 + gx) * scale, y + gy * scale,
                                  scale, scale, colour)


# 3x5 bitmap font, only the glyphs the overlay uses.
FONT = {
    "A": ["111", "101", "111", "101", "101"],
    "B": ["110", "101", "110", "101", "110"],
    "C": ["011", "100", "100", "100", "011"],
    "D": ["110", "101", "101", "101", "110"],
    "H": ["101", "101", "111", "101", "101"],
    "P": ["111", "101", "111", "100", "100"],
    "S": ["011", "100", "010", "001", "110"],
    "1": ["010", "110", "010", "010", "111"],
    "2": ["111", "001", "111", "100", "111"],
    " ": ["000", "000", "000", "000", "000"],
}


def draw_panel(cv: Canvas, ox: int, oy: int, row: Row, label: str) -> None:
    cv.rect(ox, oy, PANEL_W, PANEL_H, C_BG)
    cv.frame(ox, oy, PANEL_W, PANEL_H, C_BORDER)
    cv.text(ox + PAD, oy + 5, label, C_TEXT, scale=2)

    # --- d-pad cross ---
    dx, dy = ox + PAD, oy + 22
    cx, cy = dx + DPAD, dy + DPAD
    cv.rect(cx, dy, DPAD, DPAD, C_ON_DIR if row.dirs["up"] else C_OFF)
    cv.rect(cx, dy + DPAD * 2, DPAD, DPAD, C_ON_DIR if row.dirs["down"] else C_OFF)
    cv.rect(dx, cy, DPAD, DPAD, C_ON_DIR if row.dirs["left"] else C_OFF)
    cv.rect(dx + DPAD * 2, cy, DPAD, DPAD, C_ON_DIR if row.dirs["right"] else C_OFF)
    cv.rect(cx, cy, DPAD, DPAD, C_OFF)

    # --- buttons: two rows of three ---
    bx0 = ox + PAD + DPAD * 3 + 10
    by0 = oy + 24
    for i, b in enumerate(BUTTONS):
        col, rw = i % 3, i // 3
        bx = bx0 + col * (BTN + BTN_GAP)
        by = by0 + rw * (BTN + BTN_GAP)
        cv.rect(bx, by, BTN, BTN, C_ON_BTN if row.btns[b] else C_OFF)
        cv.text(bx + 3, by + 7, BUTTON_LABEL[b], C_TEXT, scale=1)


def render(
    video: Path,
    csv_path: Path,
    out: Path,
    *,
    start: int = 0,
    count: int = 0,
    n_cpus: int = 0,
) -> None:
    """Composite input panels onto `video[start : start+count]`."""
    from runner import throttle  # local import: pipeline must not require runner

    p1 = read_rows(csv_path, "p1")
    p2 = read_rows(csv_path, "p2")
    total = min(len(p1), len(p2))
    if total == 0:
        raise SystemExit(f"no input rows in {csv_path}")

    start = max(0, min(start, total - 1))
    count = count or (total - start)
    count = min(count, total - start)

    ov_w, ov_h = PANEL_W * 2 + PAD * 3, PANEL_H + PAD * 2

    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        # base video, trimmed to the clip
        "-ss", f"{start / FPS:.6f}", "-i", str(video),
        # overlay stream on stdin
        "-f", "rawvideo", "-pix_fmt", "bgra", "-s", f"{ov_w}x{ov_h}",
        "-r", str(FPS), "-i", "pipe:0",
        "-filter_complex",
        # bottom-centred, nudged up off the very edge
        f"[0:v][1:v]overlay=(W-w)/2:H-h-6:format=auto,format=yuv420p[v]",
        "-map", "[v]",
        "-frames:v", str(count),
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
        str(out),
    ]
    cmd = throttle.wrap(cmd, n_cpus=n_cpus)

    out.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    assert proc.stdin

    try:
        for i in range(start, start + count):
            cv = Canvas(ov_w, ov_h)
            draw_panel(cv, PAD, PAD, p1[i], "P1")
            draw_panel(cv, PAD * 2 + PANEL_W, PAD, p2[i], "P2")
            proc.stdin.write(bytes(cv.buf))
        proc.stdin.close()
    except BrokenPipeError:
        pass
    rc = proc.wait()
    if rc != 0:
        raise SystemExit(f"ffmpeg failed ({rc}) writing {out}")


def input_density(csv_path: Path, player: str = "p1") -> list[int]:
    """Per-frame count of pressed inputs. Used to find the interesting parts."""
    rows = read_rows(csv_path, player)
    return [sum(r.dirs.values()) + sum(r.btns.values()) for r in rows]


def busiest_window(csv_path: Path, length: int) -> int:
    """Start frame of the `length`-frame window with the most input activity.

    Neutral spacing dominates a fighting-game replay; picking by input density
    lands on actual exchanges instead of two characters walking.
    """
    dens = input_density(csv_path, "p1")
    d2 = input_density(csv_path, "p2")
    dens = [a + b for a, b in zip(dens, d2)]
    if len(dens) <= length:
        return 0
    window = sum(dens[:length])
    best, best_i = window, 0
    for i in range(1, len(dens) - length):
        window += dens[i + length - 1] - dens[i - 1]
        if window > best:
            best, best_i = window, i
    return best_i


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Burn a controller overlay onto a capture.")
    ap.add_argument("capture", type=Path, help="capture dir (video.mp4 + inputs.csv)")
    ap.add_argument("-o", "--out", type=Path, default=None)
    ap.add_argument("--start", type=int, default=-1,
                    help="start frame; -1 picks the busiest window")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--cpus", type=int, default=0)
    args = ap.parse_args(argv)

    video = args.capture / "video.mp4"
    csv_path = args.capture / "inputs.csv"
    for p in (video, csv_path):
        if not p.exists():
            print(f"missing {p}", file=sys.stderr)
            return 2

    count = int(args.seconds * FPS)
    start = busiest_window(csv_path, count) if args.start < 0 else args.start
    out = args.out or (args.capture / "overlay.mp4")

    print(f"rendering frames {start}..{start + count} -> {out}")
    render(video, csv_path, out, start=start, count=count, n_cpus=args.cpus)
    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
