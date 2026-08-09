"""The offset checker has to fail on a wrong offset, not just pass on a right one.

A check that only ever runs against real captures is untested in the direction
that matters. `player_state.hpp` reads four hardcoded struct offsets, and the
realistic ways they go wrong are small: off by four bytes (you get the next
field), sign inverted, or an action range shifted so guard and wrong-guard
swap. Each of those still produces a well-formed, smoothly varying CSV.

So this builds a synthetic capture where the state is correct by construction,
confirms every check passes, and then applies exactly those corruptions and
requires the corresponding check to fail.
"""

from __future__ import annotations

import csv
import math
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pipeline import verify_state  # noqa: E402

WALK = 3.0            # game units per frame, roughly Soku's walk speed
FLOOR = 0.0
COLUMNS = ["frame", "game_frame", "p1_input", "p2_input"]
BUTTONS = ["up", "down", "left", "right", "a", "b", "c", "d", "change", "spell"]
for _p in (1, 2):
    COLUMNS += [f"p{_p}_{b}" for b in BUTTONS]
for _p in (1, 2):
    COLUMNS += [f"p{_p}_x", f"p{_p}_y", f"p{_p}_dir", f"p{_p}_action",
                f"p{_p}_guarding", f"p{_p}_wrongblock", f"p{_p}_crushed",
                f"p{_p}_knockdown"]
COLUMNS += ["battle_frame"]

# The engine has been counting since the battle began, and capture arms some
# way into it. A non-zero offset is the whole point of the column, so the
# fixture has one.
BATTLE_FRAME_OFFSET = 137


def _script(n: int) -> list[tuple[str, str]]:
    """A per-frame plan for both players, cycling through the behaviours the
    checks need to see: walking each way, a jump, guarding, and a stretch where
    p2 walks far enough left to cross to p1's other side."""
    plan = []
    for i in range(n):
        phase = (i // 60) % 6
        if phase == 0:
            plan.append(("right", "left"))
        elif phase == 1:
            plan.append(("left", "right"))
        elif phase == 2:
            plan.append(("jump", "idle"))
        elif phase == 3:
            plan.append(("guard", "attack"))
        elif phase == 4:
            plan.append(("idle", "jump"))
        else:                      # p2 runs past p1: the crossup
            plan.append(("idle", "left"))
    return plan


def make_capture(path: Path, n: int = 720) -> None:
    """Write a CSV whose state columns are correct by construction."""
    x = {1: -150.0, 2: 150.0}
    y = {1: FLOOR, 2: FLOOR}
    jump_t = {1: -1, 2: -1}
    plan = _script(n)

    with path.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(COLUMNS)
        for i, (a1, a2) in enumerate(plan):
            acts = {1: a1, 2: a2}
            held = {p: {b: 0 for b in BUTTONS} for p in (1, 2)}
            for p in (1, 2):
                other = 3 - p
                # Away is the direction opposite the opponent, in world space.
                away = "left" if x[other] > x[p] else "right"
                toward = "right" if away == "left" else "left"
                act = acts[p]
                if act == "right":
                    held[p]["right"] = 1
                elif act == "left":
                    held[p]["left"] = 1
                elif act == "jump":
                    # Tapped, not held: `check_jump` counts rising edges, so a
                    # single 60-frame hold would give it one sample and the
                    # check would skip itself -- which is the same as no test.
                    if i % 20 == 0:
                        held[p]["up"] = 1
                        jump_t[p] = 0
                elif act == "guard":
                    held[p][away] = 1
                elif act == "attack":
                    held[p]["a"] = 1
                    held[p][toward] = 1

            state = {}
            for p in (1, 2):
                other = 3 - p
                # Horizontal motion is world-space: the stick moves you the way
                # it points regardless of which way you face.
                x[p] += WALK * (held[p]["right"] - held[p]["left"])
                if jump_t[p] >= 0:
                    # Two frames of prejump on the floor before rising, as the
                    # game has -- that is exactly what `check_jump` must see
                    # past, so the fixture has to contain it.
                    jump_t[p] += 1
                    t = jump_t[p] - 2
                    y[p] = (max(FLOOR, 40.0 * math.sin(math.pi * t / 13))
                            if t > 0 else FLOOR)
                    if jump_t[p] >= 15:
                        jump_t[p], y[p] = -1, FLOOR

                # A wrong block is a guard at the wrong HEIGHT, not in the
                # wrong direction (SokuLib: ACTION_WRONGBLOCK_{HIGH,LOW}_*
                # sits beside ACTION_RIGHTBLOCK_{HIGH,LOW}_*), so it happens
                # to the defender, holding away, exactly like a right block.
                blocking = acts[p] == "guard" and acts[other] == "attack"
                wrongblock = int(blocking and i % 4 == 0)
                guarding = int(blocking and not wrongblock)
                action = 150 if guarding else (159 if wrongblock else
                                               (i % 40) + 1)
                state[p] = [
                    round(x[p], 3), round(y[p], 3),
                    1 if x[other] > x[p] else -1,
                    action, guarding, wrongblock, 0,
                    int(acts[p] == "attack" and i % 97 == 0),
                ]

            row = [i, i, 0, 0]
            for p in (1, 2):
                row += [held[p][b] for b in BUTTONS]
            for p in (1, 2):
                row += state[p]
            row.append(i + BATTLE_FRAME_OFFSET)
            w.writerow(row)


def run(path: Path) -> dict[str, bool | None]:
    rows = verify_state.read_rows(path)
    checks = (verify_state.check_ranges(rows) + verify_state.check_walk(rows)
              + [verify_state.check_facing(rows)]
              + verify_state.check_guard(rows)
              + [verify_state.check_jump(rows)]
              + verify_state.check_actions(rows)
              + [verify_state.check_crossup(rows),
                 verify_state.check_battle_frame(rows)])
    return {c.name: c.ok for c in checks}


def corrupt(src: Path, dst: Path, fn) -> None:
    with src.open(newline="") as fh:
        rows = list(csv.DictReader(fh))
    for r in rows:
        fn(r)
    with dst.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=COLUMNS)
        w.writeheader()
        w.writerows(rows)


@pytest.fixture
def good(tmp_path: Path) -> Path:
    path = tmp_path / "inputs.csv"
    make_capture(path)
    return path


def test_a_correct_capture_passes_every_check(good: Path):
    results = run(good)
    bad = [k for k, v in results.items() if v is False]
    skipped = [k for k, v in results.items() if v is None]
    assert not bad, f"correct capture failed: {bad}"
    assert not skipped, f"correct capture could not exercise: {skipped}"


def test_mirrored_x_is_caught(good: Path, tmp_path: Path):
    """The sign of x inverted. Every 'away' label would be backwards -- the
    exact error the labels exist to fix, so it must not survive."""
    out = tmp_path / "mirror.csv"
    corrupt(good, out, lambda r: r.update(
        p1_x=str(-float(r["p1_x"])), p2_x=str(-float(r["p2_x"]))))
    results = run(out)
    assert results["walk_p1"] is False
    assert results["walk_p2"] is False


def test_x_offset_off_by_one_field_is_caught(good: Path, tmp_path: Path):
    """Reading four bytes late gives y where x was: both plausible floats."""
    out = tmp_path / "shifted.csv"
    corrupt(good, out, lambda r: r.update(p1_x=r["p1_y"], p2_x=r["p2_y"]))
    results = run(out)
    assert results["walk_p1"] is False or results["faces_opponent"] is False


def test_inverted_direction_is_caught(good: Path, tmp_path: Path):
    out = tmp_path / "dir.csv"
    corrupt(good, out, lambda r: r.update(
        p1_dir=str(-int(r["p1_dir"])), p2_dir=str(-int(r["p2_dir"]))))
    assert run(out)["faces_opponent"] is False


def test_a_shifted_guard_range_is_caught(good: Path, tmp_path: Path):
    """The action offset landing on a different field: `guarding` now lights up
    on frames that have nothing to do with blocking. Here it is put on the
    frames holding *toward*, which is what a mirrored or unrelated field would
    look like."""
    def shift(r):
        dx = float(r["p2_x"]) - float(r["p1_x"])
        for p, sign in ((1, 1.0), (2, -1.0)):
            toward_right = dx * sign > 0
            r[f"p{p}_guarding"] = (r[f"p{p}_right"] if toward_right
                                   else r[f"p{p}_left"])
    out = tmp_path / "shifted_guard.csv"
    corrupt(good, out, shift)
    results = run(out)
    assert results["guard_holds_away"] is False
    assert results["guard_contrast"] is False


def test_a_constant_guard_field_is_caught(good: Path, tmp_path: Path):
    """A stuck read -- every frame reported as guarding -- looks like heavy
    blocking, and `guard_holds_away` alone can be fooled by it whenever the
    match is mostly spent holding back. The contrast cannot: a constant has
    the same rate whichever direction is held, so the ratio is exactly 1."""
    def stick(r):
        for p in (1, 2):
            r[f"p{p}_guarding"] = "1"
    out = tmp_path / "stuck.csv"
    corrupt(good, out, stick)
    assert run(out)["guard_contrast"] is False


def test_inverted_y_is_caught(good: Path, tmp_path: Path):
    out = tmp_path / "y.csv"
    corrupt(good, out, lambda r: r.update(
        p1_y=str(-float(r["p1_y"])), p2_y=str(-float(r["p2_y"]))))
    assert run(out)["jump_raises_y"] is False


def test_a_battle_frame_that_is_just_the_row_number_is_caught(good: Path,
                                                             tmp_path: Path):
    """The exact trap this column was added to escape.

    `game_frame` looked like an engine tick for the life of the project and was
    a row counter -- identical to `frame` on all 6055 rows of a corpus capture
    and all 6058 of a fresh one. That made an alignment check between two
    captures vacuous: comparing 0,1,2,... to 0,1,2,... always agrees. A field
    that mirrors the row number must therefore fail, not pass for being tidy.
    """
    out = tmp_path / "rowcount.csv"
    corrupt(good, out, lambda r: r.update(battle_frame=r["frame"]))
    assert run(out)["battle_frame"] is False


def test_a_battle_frame_that_goes_backwards_is_caught(good: Path,
                                                      tmp_path: Path):
    out = tmp_path / "backwards.csv"
    corrupt(good, out, lambda r: r.update(
        battle_frame=str(BATTLE_FRAME_OFFSET + 720 - int(r["frame"]))))
    assert run(out)["battle_frame"] is False


def test_a_capture_without_state_columns_says_so(tmp_path: Path):
    path = tmp_path / "old.csv"
    with path.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["row", "game_frame", "p1_input", "p2_input"])
        for i in range(100):
            w.writerow([i, i, 0, 0])
    with pytest.raises(SystemExit, match="no state columns"):
        verify_state.read_rows(path)
