"""Check the twenty-two fields added to the sidecar against the game's own behaviour.

    python3 -m pipeline.verify_extended out/<id>/inputs.csv

WHY SEPARATELY FROM verify_state.py
-----------------------------------
`verify_state` checks the original four -- position, facing, the guard flags --
against the input columns, and those checks are what make them trustworthy.
The fields added for the state-space redesign need their own predictions, and
they are cheaper to state because most of them can be checked against EACH
OTHER: velocity against the change in position, action_frame against the action
it counts, combo_damage against the opponent's health.

That mutual redundancy is the point. A struct offset that is wrong by four
bytes reads a neighbouring field and produces numbers of the right shape, so
the test is never "is this plausible" but "does it satisfy a relationship it
could not satisfy by accident".

WHAT EACH CHECK RULES OUT
-------------------------
    speed_is_dx        vx is not the velocity, or is in different units, or
                       the position offset moved
    accel_present      ay is not gravity -- an airborne body must accelerate
    action_frame       the counter does not belong to the action beside it
    hp_range           hp is not health, or is not in the units assumed
    hp_falls_on_combo  combo_damage belongs to a different player, or hp does
    spirit_bounded     spirit and max_spirit are not a pair
    hitbox_precedes    hitboxes is not "an attack is live" -- if it is, the
                       opponent's hitstop must follow it, not precede it
    untech_on_hit      untech is not the recovery counter
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

# A tick where the engine advanced exactly one frame. Presentation can repeat a
# tick, and a repeated tick has zero position change against a non-zero speed,
# which would look like a broken offset.
def _steps(rows):
    for a, b in zip(rows, rows[1:]):
        if int(b["battle_frame"]) - int(a["battle_frame"]) == 1:
            yield a, b


class Check:
    def __init__(self, name, ok, detail):
        self.name, self.ok, self.detail = name, ok, detail

    def line(self):
        mark = "PASS" if self.ok else ("SKIP" if self.ok is None else "FAIL")
        return f"  [{mark}] {self.name:<20} {self.detail}"


def f(r, k):
    return float(r[k])


def i(r, k):
    return int(float(r[k]))


def check_speed(rows):
    """vx must explain the change in x -- in the FACING frame.

    Stated comparatively rather than as an absolute tolerance, because the
    absolute one is not the question. The game stores speed facing-relative, so
    world dx tracks vx * direction, and the residual that remains is real
    physics this field does not carry on its own: knockback and the stage-edge
    clamp are applied after it. Measured, the facing frame is four times closer
    than the world frame, which no neighbouring float would be.

    Hitstop and timestop freeze the body while speed stays set, so those ticks
    are excluded rather than counted as disagreement.
    """
    out = []
    for p in (1, 2):
        e_world = e_face = mag = 0.0
        n = 0
        for a, b in _steps(rows):
            if i(a, f"p{p}_hitstop") or i(a, f"p{p}_timestop"):
                continue
            dx = f(b, f"p{p}_x") - f(a, f"p{p}_x")
            v, d = f(a, f"p{p}_vx"), i(a, f"p{p}_dir")
            e_world += abs(dx - v)
            e_face += abs(dx - v * d)
            mag += abs(dx)
            n += 1
        if n < 100:
            out.append(Check(f"speed_facing_p{p}", None, f"only {n} clean ticks"))
            continue
        out.append(Check(f"speed_facing_p{p}", e_face < 0.5 * e_world,
                         f"err {e_face/n:.3f} in the facing frame vs "
                         f"{e_world/n:.3f} in world, mean |dx| {mag/n:.3f} "
                         f"({n} ticks)"))
    return out


def check_accel(rows):
    """An airborne character must have a non-zero downward acceleration."""
    vals = [f(r, f"p{p}_ay") for r in rows for p in (1, 2)
            if f(r, f"p{p}_y") > 1.0]
    if len(vals) < 100:
        return Check("accel_present", None, f"only {len(vals)} airborne frames")
    nz = sum(1 for v in vals if abs(v) > 1e-6) / len(vals)
    mean = sum(vals) / len(vals)
    return Check("accel_present", nz > 0.5,
                 f"{nz:.1%} of {len(vals)} airborne frames have non-zero ay, "
                 f"mean {mean:+.3f}")


def check_action_frame(rows):
    """The counter must advance with its action and reset when it changes."""
    out = []
    for p in (1, 2):
        adv = same = resets = changes = 0
        for a, b in _steps(rows):
            # Hitstop and timestop freeze the animation, so the counter
            # legitimately does not advance -- excluding them is the difference
            # between measuring the counter and measuring the freeze.
            if i(a, f"p{p}_hitstop") or i(a, f"p{p}_timestop"):
                continue
            if i(a, f"p{p}_action") == i(b, f"p{p}_action"):
                same += 1
                adv += int(i(b, f"p{p}_action_frame")
                           == i(a, f"p{p}_action_frame") + 1)
            else:
                changes += 1
                resets += int(i(b, f"p{p}_action_frame") <= 1)
        if same < 100 or changes < 20:
            out.append(Check(f"action_frame_p{p}", None, "too few transitions"))
            continue
        ok = adv / same > 0.9 and resets / changes > 0.8
        out.append(Check(f"action_frame_p{p}", ok,
                         f"advances on {adv/same:.1%} of same-action ticks, "
                         f"resets on {resets/changes:.1%} of {changes} changes"))
    return out


def check_hp(rows):
    hps = [i(r, f"p{p}_hp") for r in rows for p in (1, 2)]
    lo, hi = min(hps), max(hps)
    return Check("hp_range", 0 <= lo and hi <= 20000,
                 f"hp spans [{lo}, {hi}]; starts "
                 f"{i(rows[0],'p1_hp')}/{i(rows[0],'p2_hp')}")


def check_hp_falls_on_combo(rows):
    """When MY combo_damage rises, THEIR hp must fall.

    This is the check that couples two offsets: it fails if combo_damage
    belongs to the other player, or if hp does, or if either is not what it
    says. Nothing about a neighbouring field satisfies it by chance.
    """
    hit = agree = 0
    for a, b in _steps(rows):
        for me, them in ((1, 2), (2, 1)):
            d = i(b, f"p{me}_combo_damage") - i(a, f"p{me}_combo_damage")
            if d > 0:
                hit += 1
                agree += int(i(b, f"p{them}_hp") < i(a, f"p{them}_hp"))
    if hit < 20:
        return Check("hp_falls_on_combo", None, f"only {hit} damage ticks")
    return Check("hp_falls_on_combo", agree / hit > 0.8,
                 f"opponent's hp fell on {agree/hit:.1%} of {hit} ticks where "
                 f"combo_damage rose")


def check_spirit(rows):
    bad = tot = 0
    for r in rows:
        for p in (1, 2):
            s, m = i(r, f"p{p}_spirit"), i(r, f"p{p}_max_spirit")
            tot += 1
            bad += int(m <= 0 or s < 0 or s > m)
    return Check("spirit_bounded", bad / max(tot, 1) < 0.01,
                 f"{bad} of {tot} frames violate 0 <= spirit <= max_spirit "
                 f"(max_spirit {i(rows[0],'p1_max_spirit')})")


def check_hitbox_precedes(rows):
    """An active hitbox must come BEFORE the opponent's hitstop, not after.

    Ordering is what makes this a real test: a field correlated with combat
    would light up around a hit either way, and only the true hitbox count is
    reliably ahead of the reaction it causes.
    """
    # ONSETS only. A hitbox stays live for several frames and the hitstop it
    # causes starts partway through, so scoring every frame of an attack counts
    # the tail of the same event as "hitstop came first" -- which is what the
    # first version of this check did, reporting 230 lags against 83 leads on
    # data whose offsets are fine.
    lead = lag = 0
    W = 10
    for k in range(1, len(rows) - W):
        for me, them in ((1, 2), (2, 1)):
            onset = (i(rows[k], f"p{me}_hitboxes") > 0
                     and i(rows[k - 1], f"p{me}_hitboxes") == 0)
            if not onset:
                continue
            after = any(i(rows[j], f"p{them}_hitstop") > 0
                        for j in range(k + 1, k + W))
            before = i(rows[k - 1], f"p{them}_hitstop") > 0
            lead += int(after and not before)
            lag += int(before)
    if lead + lag < 20:
        return Check("hitbox_precedes", None, f"only {lead+lag} clean cases")
    return Check("hitbox_precedes", lead > lag,
                 f"hitstop follows an active hitbox {lead} times vs precedes "
                 f"it {lag} times")


def check_untech(rows):
    """untech must be non-zero while a player is in knockdown/hitstun."""
    kd = [r for r in rows for p in (1, 2) if i(r, f"p{p}_knockdown")]
    if len(kd) < 30:
        return Check("untech_on_hit", None, f"only {len(kd)} knockdown frames")
    on = 0
    tot = 0
    for r in rows:
        for p in (1, 2):
            if i(r, f"p{p}_knockdown"):
                tot += 1
                on += int(i(r, f"p{p}_untech") > 0)
    return Check("untech_on_hit", on / max(tot, 1) > 0.5,
                 f"untech > 0 on {on/max(tot,1):.1%} of {tot} knockdown frames")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("csv", type=Path, nargs="+")
    args = ap.parse_args(argv)
    failed = 0
    for path in args.csv:
        with path.open(newline="") as fh:
            rows = [r for r in csv.DictReader(fh)
                    if r.get("battle_frame") is not None]
        need = ("p1_vx", "p1_hp", "p1_spirit", "p1_hitboxes", "p1_untech")
        missing = [c for c in need if c not in rows[0]]
        if missing:
            raise SystemExit(f"{path}: no extended columns ({missing[:3]}); "
                             f"this capture predates the state redesign.")
        print(f"\n{path}  ({len(rows)} rows)")
        checks = (check_speed(rows) + [check_accel(rows)]
                  + check_action_frame(rows)
                  + [check_hp(rows), check_hp_falls_on_combo(rows),
                     check_spirit(rows), check_hitbox_precedes(rows),
                     check_untech(rows)])
        for c in checks:
            print(c.line())
        bad = [c for c in checks if c.ok is False]
        failed += len(bad)
        print(f"  {sum(1 for c in checks if c.ok)} passed, {len(bad)} failed, "
              f"{sum(1 for c in checks if c.ok is None)} skipped")
    if failed:
        print("\nAt least one added offset is not reading what it claims. Do "
              "NOT start a 29-hour capture on these.", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
