"""The CSV-only sink has to drain the pipe and count what it drained.

Both halves matter and neither is obvious from reading the code:

* If nothing drains the FIFO the DLL does not merely lose video -- it writes
  pixels and the CSV row for a frame back to back, so a blocked pipe stalls
  the sidecar, which is the one thing this mode exists to produce.
* The drained count replaces `validate.check_video`'s packet count as the
  independent check on the CSV's length. A counter that miscounts is worse
  than no counter, because `collect.py` fails a capture on a disagreement.

`dd` on a pipe is exactly where a naive count goes wrong: reads return at most
a pipe buffer, so without `iflag=fullblock` every record is partial and the
count is of syscalls rather than frames. That is what this pins.
"""

from __future__ import annotations

import os
import subprocess
import sys
import threading
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from runner import encode  # noqa: E402


def _feed(fifo: Path, n_frames: int) -> None:
    """Write `n_frames` raw frames, then close -- the writer side of the DLL."""
    with open(fifo, "wb") as fh:
        for i in range(n_frames):
            fh.write(bytes([i % 256]) * encode.FRAME_BYTES)


@pytest.mark.parametrize("n_frames", [1, 7])
def test_the_sink_drains_and_counts_exactly(tmp_path: Path, n_frames: int):
    fifo = tmp_path / "video.fifo"
    log = tmp_path / "ffmpeg.log"
    os.mkfifo(fifo, 0o600)

    with open(log, "wb") as fh:
        proc = subprocess.Popen(encode.build_null_command(fifo),
                                stdout=fh, stderr=subprocess.STDOUT)
        # The writer must run concurrently: opening a FIFO for writing blocks
        # until a reader attaches, and vice versa.
        w = threading.Thread(target=_feed, args=(fifo, n_frames))
        w.start()
        assert proc.wait(timeout=60) == 0
        w.join(timeout=60)

    assert encode.drained_frames(log) == n_frames


def test_a_partial_final_frame_is_not_counted_as_whole(tmp_path: Path):
    """A capture killed mid-write leaves a truncated frame on the wire.

    It must not round up into the count, or a lost frame would look like a
    complete one and the CSV cross-check would pass on damaged data.
    """
    fifo = tmp_path / "video.fifo"
    log = tmp_path / "ffmpeg.log"
    os.mkfifo(fifo, 0o600)

    def feed():
        with open(fifo, "wb") as fh:
            fh.write(b"\x01" * (encode.FRAME_BYTES * 2 + 1000))

    with open(log, "wb") as fh:
        proc = subprocess.Popen(encode.build_null_command(fifo),
                                stdout=fh, stderr=subprocess.STDOUT)
        w = threading.Thread(target=feed)
        w.start()
        assert proc.wait(timeout=60) == 0
        w.join(timeout=60)

    assert encode.drained_frames(log) == 2


def test_the_encoder_selects_the_sink_from_the_video_flag(tmp_path: Path):
    real = encode.build_command(tmp_path / "f", tmp_path / "o.mp4", vaapi=False)
    null = encode.build_null_command(tmp_path / "f")
    assert real[0] == "ffmpeg" and "libx264" in real
    assert null[0] == "dd" and "iflag=fullblock" in null
    # One frame per block, so `records in` is a frame count and not a byte one.
    assert f"bs={encode.FRAME_BYTES}" in null


def test_missing_log_reports_unknown_rather_than_zero(tmp_path: Path):
    """None and 0 must not be confused: `collect.py` fails a capture when the
    drained count disagrees with the DLL's, and an unreadable log is not
    evidence of a disagreement."""
    assert encode.drained_frames(tmp_path / "absent.log") is None
