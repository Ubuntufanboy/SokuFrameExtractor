"""Alignment has to refuse when the evidence does not determine an answer.

The failure this guards against is silent by construction. A sidecar attached
at the wrong offset still parses, still has the right number of rows, and
still trains -- it just teaches the model that a button press happened one
frame from where it did. Nothing downstream can detect that, so the only place
it can be caught is here, and the only safe behaviour is to reject rather than
return a best guess.
"""

from __future__ import annotations

import csv
import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pipeline import align_sidecar as al  # noqa: E402

STATE = [f"p{p}_{c}" for p in (1, 2) for c in
         ("x", "y", "dir", "action", "guarding", "wrongblock", "crushed",
          "knockdown")]
HEADER = ["frame", "game_frame"] + list(al.INPUT_COLS) + STATE + ["battle_frame"]


def make_rows(n: int, seed: int = 0, *, idle: bool = False) -> list[dict]:
    """A plausible input track. `idle` makes every frame identical, which is
    the case where no offset is distinguishable from any other."""
    rng = random.Random(seed)
    rows, held = [], {c: 0 for c in al.INPUT_COLS}
    for i in range(n):
        if not idle and i % 7 == 0:
            held = {c: int(rng.random() < 0.12) for c in al.INPUT_COLS}
        r = {"frame": i, "game_frame": i, "battle_frame": i + 2}
        r.update(held)
        for c in STATE:
            r[c] = 0
        r["p1_x"], r["p2_x"] = 400 + i % 50, 800 - i % 50
        rows.append(r)
    return rows


def write(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=HEADER)
        w.writeheader()
        w.writerows(rows)


def pair(tmp: Path, *, offset: int, old_n: int = 600, extra: int = 4,
         noise: int = 0, idle: bool = False) -> tuple[Path, Path]:
    """A corpus capture and a fresh one whose row i is the corpus's row i-offset.

    `offset` is what `find_offset` must recover: old[i] == new[i + offset].
    """
    base = make_rows(old_n + offset + extra, idle=idle)
    old = [dict(r) for r in base[offset:offset + old_n]]
    new = [dict(r) for r in base]
    for i, r in enumerate(old):
        r["frame"] = r["game_frame"] = i
    for i, r in enumerate(new):
        r["frame"] = r["game_frame"] = i
    rng = random.Random(99)
    for _ in range(noise):                    # frames that genuinely disagree
        j = rng.randrange(len(new))
        new[j] = dict(new[j], p1_a=1 - int(new[j]["p1_a"]))

    cdir, fdir = tmp / "corpus" / "rep-abc", tmp / "fresh" / "rep-abc"
    write(cdir / "inputs.csv", old)
    write(fdir / "inputs.csv", new)
    return cdir, fdir


@pytest.mark.parametrize("offset", [0, 1, 2, 5])
def test_the_offset_is_recovered(tmp_path: Path, offset: int):
    cdir, fdir = pair(tmp_path, offset=offset)
    rec = al.align_one(cdir, fdir, "state.csv")
    assert rec["status"] == "ok", rec
    assert rec["offset"] == offset


def test_the_written_file_is_indexed_by_video_frame(tmp_path: Path):
    """Row i of the output must be video frame i, which is the entire point."""
    cdir, fdir = pair(tmp_path, offset=3)
    assert al.align_one(cdir, fdir, "state.csv")["status"] == "ok"

    with (cdir / "inputs.csv").open(newline="") as fh:
        old = list(csv.DictReader(fh))
    with (cdir / "state.csv").open(newline="") as fh:
        aligned = list(csv.DictReader(fh))

    assert len(aligned) == len(old)
    # The inputs at row i must agree with the corpus's own row i; if the
    # alignment were off, this is where it would show.
    for i in (0, 17, len(old) // 2, len(old) - 1):
        for c in al.INPUT_COLS:
            assert aligned[i][c] == old[i][c], f"row {i}, column {c}"
    # And the state columns came along with them.
    assert "p1_guarding" in aligned[0]


def test_a_few_genuinely_differing_frames_are_tolerated(tmp_path: Path):
    """Two captures of one replay disagree on ~0.1% of rows. That must not
    look like a failed alignment."""
    cdir, fdir = pair(tmp_path, offset=1, noise=3)
    rec = al.align_one(cdir, fdir, "state.csv")
    assert rec["status"] == "ok" and rec["offset"] == 1
    assert 0 < rec["residual"] < al.MAX_RESIDUAL


def test_an_idle_track_is_refused_as_ambiguous(tmp_path: Path):
    """Every frame identical: several shifts fit perfectly and the evidence
    picks none of them. Guessing here would be a coin flip on 600 frames."""
    cdir, fdir = pair(tmp_path, offset=2, idle=True)
    rec = al.align_one(cdir, fdir, "state.csv")
    assert rec["status"] == "ambiguous", rec
    assert not (cdir / "state.csv").exists()


def test_an_unrelated_replay_is_refused(tmp_path: Path):
    """The commonest real hazard: the right id, the wrong match."""
    cdir = tmp_path / "corpus" / "rep-abc"
    fdir = tmp_path / "fresh" / "rep-abc"
    write(cdir / "inputs.csv", make_rows(600, seed=1))
    write(fdir / "inputs.csv", make_rows(600, seed=2))
    rec = al.align_one(cdir, fdir, "state.csv")
    assert rec["status"] == "residual_too_high", rec
    assert not (cdir / "state.csv").exists()


def test_a_short_fresh_capture_is_refused(tmp_path: Path):
    """Labels that stop before the video does would train on a truncated
    track with nothing marking where it ended."""
    cdir, fdir = pair(tmp_path, offset=0, extra=0)
    with (fdir / "inputs.csv").open(newline="") as fh:
        rows = list(csv.DictReader(fh))
    write(fdir / "inputs.csv", rows[:-50])
    rec = al.align_one(cdir, fdir, "state.csv")
    assert rec["status"] == "incomplete_coverage", rec
    assert not (cdir / "state.csv").exists()


def test_dry_run_writes_nothing(tmp_path: Path):
    cdir, fdir = pair(tmp_path, offset=1)
    assert al.align_one(cdir, fdir, "state.csv", dry_run=True)["status"] == "ok"
    assert not (cdir / "state.csv").exists()
