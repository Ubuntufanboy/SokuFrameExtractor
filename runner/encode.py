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
    crf: int = 23,
    vaapi_device: str = "/dev/dri/renderD128",
) -> list[str]:
    """FFmpeg argv for reading raw BGRA off `fifo` into `out_mp4`.

    `vflip` is not cosmetic: glReadPixels returns rows bottom-up, so without it
    every captured frame is upside down.
    """
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "rawvideo",
        "-pix_fmt", "bgra",
        "-s", f"{GAME_W}x{GAME_H}",
        "-r", str(FPS),
        "-i", str(fifo),
    ]
    if vaapi:
        cmd += [
            "-vaapi_device", vaapi_device,
            "-vf", "vflip,format=nv12,hwupload",
            "-c:v", "h264_vaapi", "-qp", str(crf),
        ]
    else:
        cmd += [
            "-vf", "vflip",
            "-c:v", "libx264",
            "-preset", "veryfast",   # capture is CPU-bound; don't fight the game for cores
            "-crf", str(crf),
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
    crf: int = 23
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
            build_command(self.fifo, self.out_mp4, vaapi=self.vaapi, crf=self.crf),
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
