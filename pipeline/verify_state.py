"""Decide whether the game-state columns are actually the game's state.

    python3 -m pipeline.verify_state out/<replay_id>/inputs.csv

WHY THIS EXISTS
---------------
`dll/include/sfe/player_state.hpp` reads four hardcoded offsets out of the
character object. They come from SokuLib's documented layout, which is
excellent evidence and not proof: a struct offset that is wrong by four bytes
reads a different float and produces numbers that are the right *shape* --
finite, smoothly varying, plausible on a plot. Every downstream consumer would
accept them. `sokubot/data/state.py` would train a supervised head against
them. Nothing would fail.

So the offsets are checked against something already known to be correct: the
input columns, which have been validated since the first capture. If `x` is
really a horizontal position then holding RIGHT must move it right, and if
`direction` is really the facing flag it must point at the other player. Those
predictions are not satisfiable by a wrong offset that merely looks reasonable.

WHAT EACH CHECK RULES OUT
-------------------------
    walk_direction    x is not a position, or not horizontal, or its sign is
                      flipped (which would inverted every "away" label)
    faces_opponent    direction is not the facing flag, or x is wrong -- the
                      check couples them, so it fails if either is
    guard_holds_away  the action-ID ranges are misread, or the whole guard
                      story is wrong. This is the one the world model needs.
    guard_contrast    the mirror prediction. Holding away must produce guard
                      far more often than holding toward; a field stuck at 1
                      passes `guard_holds_away` by accident and cannot produce
                      a contrast, because a constant has the same rate in both
                      conditions.
    jump_raises_y     y is not height, or is inverted
    crossup_present   the replay never swaps sides, so it cannot exercise the
                      mechanic and a pass means little either way

WRONGBLOCK IS NOT A DIRECTION ERROR
-----------------------------------
This file first predicted that wrong-block frames would hold *toward* the
opponent, on the reading that guarding the wrong way means facing the wrong
way. The first real capture said otherwise -- 96.4% of them hold away -- and
SokuLib settles it: the range is ACTION_WRONGBLOCK_{HIGH,LOW}_*_BLOCKSTUN,
sitting beside ACTION_RIGHTBLOCK_{HIGH,LOW}_*. HIGH/LOW is the height the
block was made at, so a wrong block is a correctly *aimed* guard at the wrong
height, and the defender holds away in both. The check was wrong, not the
offset; `guard_contrast` now carries the mirror prediction instead, and
`wrongblock_holds_away` keeps what the measurement actually established.

Stdlib only, so it runs inside the collection sandbox next to the capture.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path

# Thresholds are stated here, before any data is read, because the failure mode
# this whole file exists to catch is a number that looks fine once you have
# seen it. Each is loose: a correct read clears them by a wide margin, and
# nothing near the boundary should be called a pass.
MIN_FACING_AGREEMENT = 0.80   # chars turn to face; some frames are mid-turn
# Both measured over frames where a direction was actually held; see
# `_away_fraction`. Observed 89-98% and 97-100% across seven captures, against
# a 46-67% base rate, so 0.80 is loose without being meaningless.
MIN_GUARD_AWAY       = 0.80
MIN_WRONGBLOCK_AWAY  = 0.80   # a wrong block is a guard at the wrong HEIGHT
MIN_GUARD_CONTRAST   = 4.0    # times more guard while holding away than toward
MIN_ACTIONS_SEEN     = 15     # a real match visits far more than this
MAX_ACTION_ID        = 1000   # SokuLib's table ends well below this
MIN_GUARD_FRAMES     = 30     # below this the guard checks are not evidence
STAGE_SPAN_LIMIT     = 2400.0 # |dx| beyond this is not a position at all
JUMP_WINDOW          = 12     # frames; Soku's prejump alone is about four


class Check:
    def __init__(self, name: str, ok: bool | None, detail: str,
                 stats: tuple[int, int] | None = None):
        self.name, self.ok, self.detail = name, ok, detail
        # (numerator, denominator) for the proportion checks, so a run over
        # many captures can POOL them. This matters more than it looks: an
        # offset is a property of the extractor, not of one replay, and a
        # single match where the defender let go of the stick during 74 frames
        # of blockstun says nothing about whether CHAR_POSITION_X_OFFSET is
        # right. Pooled over 2003 captures the same question has millions of
        # frames behind it.
        self.stats = stats

    def line(self) -> str:
        mark = "PASS" if self.ok else ("SKIP" if self.ok is None else "FAIL")
        return f"  [{mark}] {self.name:<18} {self.detail}"


def read_rows(path: Path) -> list[dict]:
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        cols = reader.fieldnames or []
        need = [f"p{i}_{c}" for i in (1, 2)
                for c in ("x", "y", "dir", "action", "guarding",
                          "wrongblock", "crushed", "knockdown")]
        missing = [c for c in need if c not in cols]
        if missing:
            raise SystemExit(
                f"{path}: no state columns ({missing[:3]}...). This capture "
                f"was made by an extractor built before player_state.cpp.")
        # A killed capture can leave a half-written final line; DictReader pads
        # it with None, which would otherwise surface as a TypeError deep in a
        # statistic and say nothing about the cause.
        rows = [r for r in reader if r.get("p2_knockdown") is not None]
    if len(rows) < 60:
        raise SystemExit(f"{path}: only {len(rows)} complete rows; too short "
                         f"to say anything about the offsets.")
    return rows


def _f(row: dict, key: str) -> float:
    return float(row[key])


def _i(row: dict, key: str) -> int:
    return int(float(row[key]))


def check_ranges(rows: list[dict]) -> list[Check]:
    out = []
    for p in (1, 2):
        xs = [_f(r, f"p{p}_x") for r in rows]
        ys = [_f(r, f"p{p}_y") for r in rows]
        out.append(Check(
            f"x_range_p{p}",
            max(abs(min(xs)), abs(max(xs))) < STAGE_SPAN_LIMIT,
            f"x in [{min(xs):.0f}, {max(xs):.0f}], y in [{min(ys):.0f}, "
            f"{max(ys):.0f}]"))
    dxs = [_f(r, "p2_x") - _f(r, "p1_x") for r in rows]
    out.append(Check(
        "dx_range",
        max(abs(min(dxs)), abs(max(dxs))) < STAGE_SPAN_LIMIT,
        f"separation in [{min(dxs):.0f}, {max(dxs):.0f}]"))
    return out


def check_walk(rows: list[dict]) -> list[Check]:
    """Holding RIGHT must increase x, holding LEFT must decrease it.

    World-space, not facing-relative: in Hisoutensoku the stick moves you
    toward the direction pressed whichever way you face, so this is a direct
    statement about the coordinate and needs no knowledge of the character.

    Frames where both or neither horizontal direction is held are dropped
    rather than counted as zero -- most frames are neither, and including them
    would drown the effect in a mean over mostly-standing-still.
    """
    out = []
    for p in (1, 2):
        right, left = [], []
        for a, b in zip(rows, rows[1:]):
            dx = _f(b, f"p{p}_x") - _f(a, f"p{p}_x")
            r, l = _i(a, f"p{p}_right"), _i(a, f"p{p}_left")
            if r and not l:
                right.append(dx)
            elif l and not r:
                left.append(dx)
        if len(right) < 20 or len(left) < 20:
            out.append(Check(f"walk_p{p}", None,
                             f"only {len(right)} right / {len(left)} left "
                             f"frames; not enough to test"))
            continue
        mr = sum(right) / len(right)
        ml = sum(left) / len(left)
        out.append(Check(
            f"walk_p{p}", mr > 0 > ml,
            f"mean dx {mr:+.2f}/frame holding right, {ml:+.2f} holding left "
            f"({len(right)}/{len(left)} frames)"))
    return out


def check_facing(rows: list[dict]) -> Check:
    """`direction` must point at the other player.

    Couples the direction offset to the x offset: it can only pass if both are
    right, which is why one check covers two fields.
    """
    agree = total = 0
    for r in rows:
        dx = _f(r, "p2_x") - _f(r, "p1_x")
        if dx == 0:
            continue
        for p, sign in ((1, 1.0), (2, -1.0)):
            d = _i(r, f"p{p}_dir")
            if d == 0:
                continue
            total += 1
            # p1 faces right when p2 is to its right; p2 is the mirror.
            agree += int((d > 0) == (dx * sign > 0))
    if not total:
        return Check("faces_opponent", None, "direction is 0 on every frame")
    frac = agree / total
    return Check("faces_opponent", frac >= MIN_FACING_AGREEMENT,
                 f"{frac:.1%} of {total} frames face the opponent "
                 f"(threshold {MIN_FACING_AGREEMENT:.0%})",
                 stats=(agree, total))


def _away_fraction(rows: list[dict], flag: str) -> tuple[int, float]:
    """Of block frames where a direction WAS held, how often was it away?

    Away is defined from positions alone -- opponent to my right means away is
    left -- so this is the mechanic stated in exactly the terms the world model
    could not learn from pixels.

    Frames holding neither direction (or both) are excluded, and that is not a
    detail. Blockstun is precisely when a player lets go of the stick: the
    block is already committed and holding it changes nothing. Counting those
    frames as "did not hold away" measured stick-release, not direction, and
    it dragged one replay's wrong-block figure to 60.9% -- below the threshold,
    reported as a failing offset. With the denominator corrected, guarding runs
    89-98% across seven captures and wrong-block 97-100%, against a base rate
    of 46-67% for holding away at all.
    """
    hit = held = 0
    for r in rows:
        dx = _f(r, "p2_x") - _f(r, "p1_x")
        if dx == 0:
            continue
        for p, sign in ((1, 1.0), (2, -1.0)):
            if not _i(r, f"p{p}_{flag}"):
                continue
            left, right = _i(r, f"p{p}_left"), _i(r, f"p{p}_right")
            if left == right:          # nothing held, or both: says nothing
                continue
            hit += 1
            held += left if dx * sign > 0 else right
    return hit, (held / hit if hit else 0.0)


def check_guard(rows: list[dict]) -> list[Check]:
    n_guard, away = _away_fraction(rows, "guarding")
    if n_guard < MIN_GUARD_FRAMES:
        guard = Check("guard_holds_away", None,
                      f"only {n_guard} guarding frames; nobody blocked enough "
                      f"in this replay to test")
    else:
        guard = Check("guard_holds_away", away >= MIN_GUARD_AWAY,
                      f"{away:.1%} of {n_guard} guarding frames hold away "
                      f"(threshold {MIN_GUARD_AWAY:.0%})",
                      stats=(round(away * n_guard), n_guard))

    n_wrong, wrong_away = _away_fraction(rows, "wrongblock")
    if n_wrong < 10:
        wrong = Check("wrongblock_away", None,
                      f"only {n_wrong} wrong-block frames")
    else:
        wrong = Check("wrongblock_away", wrong_away >= MIN_WRONGBLOCK_AWAY,
                      f"{wrong_away:.1%} of {n_wrong} wrong-block frames hold "
                      f"away (threshold {MIN_WRONGBLOCK_AWAY:.0%}) -- a wrong "
                      f"block is the right direction at the wrong height",
                      stats=(round(wrong_away * n_wrong), n_wrong))
    return [guard, wrong, check_guard_contrast(rows)]


def check_guard_contrast(rows: list[dict]) -> Check:
    """Guard must be far commoner while holding away than while holding toward.

    This is the check a broken read cannot fake. `guard_holds_away` asks what
    the direction was *given* a guard frame, so a `guarding` field stuck at 1
    scores whatever fraction of the match was spent holding back -- often high
    enough to pass. A ratio between two conditions cannot be produced by a
    constant at all: stuck at 1 gives exactly 1.0, and so does stuck at 0.
    """
    counts = {"away": [0, 0], "toward": [0, 0]}   # [guarding, total]
    for r in rows:
        dx = _f(r, "p2_x") - _f(r, "p1_x")
        if dx == 0:
            continue
        for p, sign in ((1, 1.0), (2, -1.0)):
            left, right = _i(r, f"p{p}_left"), _i(r, f"p{p}_right")
            if left == right:            # both or neither: no direction held
                continue
            toward_right = dx * sign > 0
            holding_away = bool(left) if toward_right else bool(right)
            key = "away" if holding_away else "toward"
            counts[key][1] += 1
            counts[key][0] += _i(r, f"p{p}_guarding")

    if min(counts["away"][1], counts["toward"][1]) < 100:
        return Check("guard_contrast", None,
                     f"only {counts['away'][1]} away / "
                     f"{counts['toward'][1]} toward frames")
    p_away = counts["away"][0] / counts["away"][1]
    p_toward = counts["toward"][0] / counts["toward"][1]
    if p_toward == 0:
        return Check("guard_contrast", p_away > 0,
                     f"guard on {p_away:.1%} of away frames, never on the "
                     f"{counts['toward'][1]} toward frames")
    ratio = p_away / p_toward
    return Check("guard_contrast", ratio >= MIN_GUARD_CONTRAST,
                 f"guard {p_away:.1%} holding away vs {p_toward:.1%} holding "
                 f"toward = {ratio:.1f}x (threshold {MIN_GUARD_CONTRAST:.0f}x)")


def check_jump(rows: list[dict]) -> Check:
    """Pressing UP from the ground must raise y, on at least one player.

    Two restrictions, both learned from the first real capture, where a naive
    version measured -1.1 and called a perfectly good offset broken:

    * **Only presses made on the ground count.** Up is held constantly in the
      air -- to double jump, to fly, to drift -- and those frames are as often
      on the way down as up. Grounded presses are the only ones that make a
      prediction about y at all.
    * **The peak over a window, not the value at a fixed offset.** Soku has a
      four-frame prejump during which the character is still on the floor, so
      sampling at +4 lands in the startup and reads zero rise.
    """
    best = None
    for p in (1, 2):
        rises = []
        for i, r in enumerate(rows[:-JUMP_WINDOW]):
            # Rising edge, from the floor: a held UP counts once, not once per
            # frame, so one long jump cannot dominate the mean.
            if not (i and _i(r, f"p{p}_up") and not _i(rows[i - 1], f"p{p}_up")):
                continue
            y0 = _f(r, f"p{p}_y")
            if y0 > 1.0:
                continue
            peak = max(_f(rows[i + k], f"p{p}_y")
                       for k in range(1, JUMP_WINDOW + 1))
            rises.append(peak - y0)
        if len(rises) >= 5:
            mean = sum(rises) / len(rises)
            if best is None or mean > best[0]:
                best = (mean, p, len(rises))
    if best is None:
        return Check("jump_raises_y", None, "no grounded up-presses to test")
    mean, p, n = best
    return Check("jump_raises_y", mean > 0,
                 f"p{p} peaks {mean:+.1f} y within {JUMP_WINDOW} frames of "
                 f"{n} grounded up-presses")


def check_actions(rows: list[dict]) -> list[Check]:
    seen: Counter = Counter()
    for r in rows:
        for p in (1, 2):
            seen[_i(r, f"p{p}_action")] += 1
    top = ", ".join(f"{a}x{n}" for a, n in seen.most_common(5))
    return [
        Check("action_range", max(seen) < MAX_ACTION_ID,
              f"{len(seen)} distinct ids, max {max(seen)}; top: {top}"),
        Check("action_variety", len(seen) >= MIN_ACTIONS_SEEN,
              f"{len(seen)} distinct action ids "
              f"(threshold {MIN_ACTIONS_SEEN})"),
    ]


def clock_phases(rows: list[dict]) -> list[tuple[int, int]]:
    """Split rows into runs over which `battle_frame` increases.

    `BattleManager.frameCount` is NOT a match clock. It restarts at every
    battle sub-state, so one capture of a two-round match looks like:

        rows    0..58    bf    2..60     intro (capture armed 2 ticks in)
        rows   59..179   bf    0..120    round 1 countdown
        rows  180..3769  bf    0..3589   round 1
        rows 3770..4061  bf    0..291    the KO sequence
        rows 4062..4182  bf    0..120    round 2 countdown
        rows 4183..5657  bf    0..1474   round 2
        rows 5658..6060  bf    0..353    results

    Returns [(start_row, end_row), ...] inclusive. The run *lengths* are a
    property of the replay rather than of the capture, which is what makes
    them usable to align two captures of one replay.
    """
    bf = [_i(r, "battle_frame") for r in rows]
    out, start = [], 0
    for i in range(1, len(bf)):
        if bf[i] < bf[i - 1]:
            out.append((start, i - 1))
            start = i
    out.append((start, len(bf) - 1))
    return out


def check_battle_frame(rows: list[dict]) -> Check:
    """`battle_frame` must be the engine's own clock, not another row counter.

    That distinction is the entire reason the column exists, and it is not
    hypothetical: `game_frame` was documented as the engine tick for the life
    of the project and is in fact identical to `frame` on every row of every
    capture ever taken. An alignment check built on it compares 0,1,2,... with
    0,1,2,... and always agrees, which is how two captures of replay 5314129
    could be a frame apart with nothing noticing.

    So the test is that the column carries information the row number does not,
    advances about one tick per row inside each phase, and resets only at phase
    boundaries -- a handful of times, not continuously.
    """
    if "battle_frame" not in rows[0]:
        return Check("battle_frame", None,
                     "column absent; capture predates the engine clock")
    bf = [_i(r, "battle_frame") for r in rows]
    if all(v == i for i, v in enumerate(bf)):
        return Check("battle_frame", False,
                     "identical to the row index on every row, so it carries "
                     "nothing an alignment could use")

    phases = clock_phases(rows)
    # Within a phase the clock must not run backwards, and must advance at
    # roughly one tick per row -- the presentation can double or drop a frame,
    # but it cannot systematically outrun or lag the engine.
    ratios = [(bf[b] - bf[a] + 1) / (b - a + 1) for a, b in phases
              if b - a + 1 >= 30]
    ok = (1 <= len(phases) <= 20
          and all(0.75 <= r <= 1.25 for r in ratios))
    span = ", ".join(f"{b - a + 1}r/{bf[b] - bf[a] + 1}t" for a, b in phases)
    return Check("battle_frame", ok,
                 f"starts at {bf[0]}, {len(phases)} phases [{span}]")


def check_crossup(rows: list[dict]) -> Check:
    signs = [1 if _f(r, "p2_x") - _f(r, "p1_x") > 0 else -1 for r in rows]
    flips = sum(1 for a, b in zip(signs, signs[1:]) if a != b)
    return Check("crossup_present", flips > 0,
                 f"{flips} side swaps; without one the away-side mechanic is "
                 f"never exercised")


def summarise(rows: list[dict]) -> str:
    n = len(rows)
    parts = []
    for flag in ("guarding", "wrongblock", "crushed", "knockdown"):
        c = sum(_i(r, f"p{p}_{flag}") for r in rows for p in (1, 2))
        parts.append(f"{flag} {c} ({c / (2 * n):.1%})")
    return f"{n} frames; " + ", ".join(parts)


# Pooled thresholds. These are not the per-capture ones relaxed -- they are a
# different question. A per-capture threshold asks "is this replay consistent
# with the offsets", where a defender releasing the stick through 74 frames of
# blockstun is ordinary and drags the figure down. The pooled one asks "are the
# offsets right", over millions of frames, where the answer should be
# overwhelming or something is wrong.
POOLED_MIN = {"faces_opponent": 0.90, "guard_holds_away": 0.90,
              "wrongblock_away": 0.90}


def summarise(paths: list[Path]) -> int:
    """One verdict for a whole corpus, pooling every capture's raw counts."""
    pooled: dict[str, list[int]] = {}
    per_capture: dict[str, list[float]] = {}
    n_read = n_bad = 0
    for path in paths:
        try:
            rows = read_rows(path)
        except SystemExit as e:
            n_bad += 1
            print(f"unreadable: {path}: {e}", file=sys.stderr)
            continue
        n_read += 1
        for c in ([check_facing(rows)] + check_guard(rows)):
            if c.stats is None or c.stats[1] == 0:
                continue
            num, den = c.stats
            acc = pooled.setdefault(c.name, [0, 0])
            acc[0] += num
            acc[1] += den
            per_capture.setdefault(c.name, []).append(num / den)

    print(f"pooled over {n_read} captures" + (f" ({n_bad} unreadable)"
                                              if n_bad else ""))
    print(f"\n  {'check':<18} {'pooled':>8} {'frames':>12}   "
          f"{'per-capture p5':>14} {'p50':>7}")
    ok = True
    for name, (num, den) in sorted(pooled.items()):
        v = sorted(per_capture[name])
        p5 = v[int(len(v) * 0.05)] if v else float("nan")
        p50 = v[len(v) // 2] if v else float("nan")
        frac = num / den
        floor = POOLED_MIN.get(name, 0.0)
        ok &= frac >= floor
        mark = "" if frac >= floor else "   <-- BELOW %.0f%%" % (floor * 100)
        print(f"  {name:<18} {frac:7.2%} {den:12,}   {p5:13.1%} {p50:6.1%}{mark}")

    print("\n" + ("The offsets are consistent with the inputs across the whole "
                  "corpus." if ok else
                  "A POOLED figure is below its floor. That is not per-capture "
                  "noise; check the offsets."))
    return 0 if ok else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Check the state columns against the input columns.")
    ap.add_argument("csv", type=Path, nargs="+",
                    help="inputs.csv from a capture (or several)")
    ap.add_argument("--summary", action="store_true",
                    help="pool the proportion checks across every capture and "
                         "report one verdict instead of one per capture. This "
                         "is the right instrument for a corpus: see the note "
                         "on Check.stats.")
    args = ap.parse_args(argv)

    if args.summary:
        return summarise(args.csv)

    failed = 0
    for path in args.csv:
        rows = read_rows(path)
        print(f"\n{path}")
        print(f"  {summarise(rows)}\n")
        checks = (check_ranges(rows) + check_walk(rows) + [check_facing(rows)]
                  + check_guard(rows) + [check_jump(rows)]
                  + check_actions(rows) + [check_crossup(rows),
                                           check_battle_frame(rows)])
        for c in checks:
            print(c.line())
        bad = [c for c in checks if c.ok is False]
        skipped = [c for c in checks if c.ok is None]
        failed += len(bad)
        print(f"\n  {len(checks) - len(bad) - len(skipped)} passed, "
              f"{len(bad)} failed, {len(skipped)} skipped")

    if failed:
        print("\nThe offsets in dll/include/sfe/player_state.hpp are not "
              "reading what they claim to. Do not train on these labels.",
              file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
