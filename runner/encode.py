"""FIFO lifecycle and the FFmpeg process that drains it.

The DLL writes raw BGRA frames into a named pipe; FFmpeg reads the other end
and produces an mp4. Two properties of that arrangement drive the design here:

* **FFmpeg must be attached before the game starts.** Opening a FIFO for
  writing blocks until a reader exists. The DLL opens it on its encoder thread
  precisely so a missing reader cannot freeze the game, but if nothing ever
  attaches, the capture stalls and the run is wasted. Start FFmpeg first.

* **Closing the write end is the only EOF FFmpeg gets.** The DLL closes the
  FIFO when it finishes draining, which is what lets FFmpeg finalise the mp4
  moov atom. If the game is killed instead, FFmpeg sits on a pipe that never
  closes, so every wait here is bounded.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

from . import throttle

GAME_W, GAME_H = 640, 480
FPS = 60

# -------------------------------------------------------------------------
# Output geometry
# -------------------------------------------------------------------------
# The game renders 640x480; the dataset wants 1:1. The frame is *squashed*
# to a square rather than padded or cropped:
#
#   pad  -> 25% of every frame is black. Cheap in H.264, but the pixels still
#           cost a tensor slot at training time.
#   crop -> loses the left and right edges, which is exactly where the two
#           players are during zoning. Destroys the behaviour worth learning.
#   squash -> keeps every pixel, wastes none, and distorts horizontally by a
#           constant 0.75. A constant is something a model absorbs for free.
#
# 480 keeps the full vertical resolution, so the only resampling is
# horizontal (640 -> 480). Lanczos rather than the bilinear default: Soku's
# art is high-contrast pixel work with 1px outlines, and bilinear visibly
# muddies it at this ratio.
SQUARE = 480

# -------------------------------------------------------------------------
# Bitrate budget
# -------------------------------------------------------------------------
# The requirement is a hard ceiling of 700 MB per hour of footage:
#
#   700 MB/h = 700e6 * 8 / 3600 bits/s = 1.556 Mbit/s
#
# CRF alone cannot promise that -- it targets quality and lets the bitrate go
# where it must. So CRF sets the quality and a VBV cap makes the ceiling
# real: over any window of `bufsize` bits the rate cannot exceed `maxrate`,
# which bounds the file for any duration.
#
# The cap is set below the budget, not at it, so the container overhead and
# the mp4 index still fit underneath. It only engages during the busiest
# scenes; typical footage lands well under it.
#
# Measured on a real 302 s capture, squashed to 480x480:
#
#   preset      crf   MB/hour   encode fps
#   medium      26        681           97
#   medium      28        524          137
#   fast        26        689          152
#   veryfast    26        625          246   <- default
#   veryfast    24       (higher)      ...
#
# veryfast/26 is the choice: 625 MB/h leaves ~11% headroom under the budget,
# and 246 fps is comfortably ahead of the ~95 fps the capture produces. The
# encoder must outrun the game or the ring buffer back-pressures the render
# thread -- which is why `medium` is not used despite compressing better.
MB_PER_HOUR_BUDGET = 700
MAXRATE_KBIT = 1500          # 675 MB/h ceiling
BUFSIZE_KBIT = 3000          # 2 s VBV window
DEFAULT_CRF = 26


def has_vaapi(device: str = "/dev/dri/renderD128") -> bool:
    """True if VAAPI hardware encoding looks available.

    Checked at runtime rather than read from a config flag: the old .ini had a
    `UseVAAPI` option that no code ever consulted, so the setting was a
    fiction. Capability belongs to the machine, not to a config file.
    """
    if not Path(device).exists():
        return False
    try:
        res = subprocess.run(
            ["ffmpeg", "-hide_banner", "-encoders"],
            capture_output=True, text=True, timeout=30,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False
    return "h264_vaapi" in res.stdout


def build_command(
    fifo: Path,
    out_mp4: Path,
    *,
    vaapi: bool,
    crf: int = DEFAULT_CRF,
    square: int = SQUARE,
    threads: int = 4,
    vaapi_device: str = "/dev/dri/renderD128",
) -> list[str]:
    """FFmpeg argv for reading raw BGRA off `fifo` into a square `out_mp4`."""
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "rawvideo",
        "-pix_fmt", "bgra",
        "-s", f"{GAME_W}x{GAME_H}",
        "-r", str(FPS),
        "-i", str(fifo),
    ]
    # vflip is not cosmetic (glReadPixels is bottom-up); the scale is the
    # 4:3 -> 1:1 squash described at the top of this module.
    scale = f"scale={square}:{square}:flags=lanczos"

    if vaapi:
        cmd += [
            "-vaapi_device", vaapi_device,
            "-vf", f"vflip,{scale},format=nv12,hwupload",
            "-c:v", "h264_vaapi", "-qp", str(crf),
            "-maxrate", f"{MAXRATE_KBIT}k", "-bufsize", f"{BUFSIZE_KBIT}k",
        ]
    else:
        cmd += [
            "-vf", f"vflip,{scale}",
            "-c:v", "libx264",
            "-preset", "veryfast",   # must outrun capture; see the note above
            "-crf", str(crf),
            "-maxrate", f"{MAXRATE_KBIT}k",
            "-bufsize", f"{BUFSIZE_KBIT}k",
            "-g", "120",             # 2 s GOP: keeps the VBV cap enforceable
            # x264 otherwise sizes its thread pool from the machine, not from
            # the cpuset it was given, so ten concurrent encoders would each
            # spawn ~48 threads onto four pinned cores.
            "-threads", str(max(1, threads)),
            "-pix_fmt", "yuv420p",   # required for broad playback compatibility
        ]
    cmd += ["-movflags", "+faststart", str(out_mp4)]
    return cmd


@dataclass
class Encoder:
    """An FFmpeg process bound to a FIFO, scoped to a `with` block."""

    fifo: Path
    out_mp4: Path
    vaapi: bool = False
    crf: int = DEFAULT_CRF
    square: int = SQUARE
    log_path: Path | None = None
    n_cpus: int = 0

    _proc: subprocess.Popen | None = None
    _log = None

    def __enter__(self) -> "Encoder":
        if not shutil.which("ffmpeg"):
            raise RuntimeError("ffmpeg not found on PATH")

        # A stale FIFO from a killed run would silently connect the new game to
        # the old reader's leftovers.
        if self.fifo.exists():
            self.fifo.unlink()
        self.fifo.parent.mkdir(parents=True, exist_ok=True)
        os.mkfifo(self.fifo, 0o600)

        self.out_mp4.parent.mkdir(parents=True, exist_ok=True)

        # x264 defaults to one thread per core and would otherwise compete with
        # the game's software rasteriser for the same cores.
        argv = throttle.wrap(
            build_command(self.fifo, self.out_mp4, vaapi=self.vaapi,
                          crf=self.crf, square=self.square,
                          threads=self.n_cpus or 4),
            n_cpus=self.n_cpus,
        )

        self._log = open(self.log_path, "wb") if self.log_path else subprocess.DEVNULL
        self._proc = subprocess.Popen(
            argv,
            stdout=self._log,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
        )
        return self

    def wait(self, timeout_s: float) -> bool:
        """Wait for FFmpeg to finish after the writer closed. True if it exited."""
        assert self._proc is not None
        try:
            self._proc.wait(timeout=timeout_s)
            return True
        except subprocess.TimeoutExpired:
            return False

    @property
    def returncode(self) -> int | None:
        return self._proc.poll() if self._proc else None

    def __exit__(self, *exc) -> None:
        if self._proc and self._proc.poll() is None:
            # Nobody closed the write end (the game was killed), so FFmpeg is
            # blocked on read forever. Open and close the FIFO ourselves to
            # deliver the EOF that lets it finalise whatever it already has --
            # a truncated-but-valid mp4 beats a zero-byte file when
            # diagnosing what went wrong.
            try:
                fd = os.open(self.fifo, os.O_WRONLY | os.O_NONBLOCK)
                os.close(fd)
            except OSError:
                pass
            if not self.wait(15):
                self._proc.terminate()
                try:
                    self._proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    self._proc.kill()

        if self._log not in (None, subprocess.DEVNULL):
            self._log.close()
        if self.fifo.exists():
            self.fifo.unlink()
