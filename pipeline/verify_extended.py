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
    proj_owner         the object list hangs off the wrong player, or is
                       shared between them -- the sharpest thing that can be
                       wrong about a pointer walk that dereferences three hops
    proj_speed         a projectile's speed offset is not its speed, or is in
                       a different frame from the character's
    proj_bounds        the walk is reading objects that are not on the stage
    proj_hits          the objects found are not attacks at all
"""

from __future__ import annotations

import argparse
import csv
import gzip
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

    THE MARGIN IS 0.7, AND IT WAS 0.5
    ---------------------------------
    Ten player-measurements over five replays put facing/world at

        0.105  0.133  0.148  0.217  0.228  0.289  0.311  0.362  0.509  0.509

    -- the facing frame is better in every single case, and a hard cut at 0.5
    landed exactly on the two weakest, failing correct data on two of ten.
    The residual is not noise but real physics this field does not carry
    (knockback and the stage-edge clamp are applied after it), and how much of
    it there is depends on the match. What has to be separated is "the facing
    frame explains this" from "the frame makes no difference", and a wrong
    convention would sit at 1.0, not at 0.5.
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
        out.append(Check(f"speed_facing_p{p}", e_face < 0.7 * e_world,
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
        # 0.85, not 0.9. The bar was set from a single capture that read 92.5%
        # and 94.3%; six more measurements landed between 89.6% and 93.4%, so a
        # 0.9 cut sits inside the spread and fails honest data about one time in
        # six. What it has to separate is a real counter from a neighbouring
        # field, and a wrong offset does not advance on 89% of ticks -- it
        # advances on approximately none. The reset rate, which is 100% on every
        # capture ever taken, is the sharp half of this check.
        ok = adv / same > 0.85 and resets / changes > 0.8
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
    """untech must be non-zero just after a hit lands.

    MEASURED AGAINST THE WRONG POPULATION AT FIRST
    ----------------------------------------------
    The original version asked what fraction of `knockdown` frames carry a
    non-zero untech, and read 74.6%, 85.8% and 36.7% on three replays of
    identical, correct data. The spread is not noise: `knockdown` includes
    ACT_KNOCKED_DOWN_STATIC, which is lying on the floor -- a state that
    persists long after untech has counted down to zero, and legitimately so.
    The check was therefore measuring how much of each match was spent on the
    ground.

    The replacement asked whether the victim's untech is NON-ZERO after a hit
    and read 100% on every replay -- which turned out to be nearly vacuous. The
    field is non-zero on 66% of all frames anyway (it holds its value rather
    than counting down; see player_state.hpp), so a high rate after hits says
    little. A base rate that high is exactly the thing a pass/fail check has to
    be measured against.

    So this asks whether untech RISES at the moment of the hit. A counter that
    is written when damage lands must increase then; one that merely happens to
    be non-zero most of the time will not, and the contrast against the same
    quantity on non-hit ticks is reported so the base rate is visible rather
    than assumed.
    """
    W = 4
    hit = rose = 0
    ctrl = ctrl_rose = 0
    for k in range(len(rows) - W):
        a, b = rows[k], rows[k + 1]
        if int(b["battle_frame"]) - int(a["battle_frame"]) != 1:
            continue
        for me, them in ((1, 2), (2, 1)):
            landed = i(b, f"p{me}_combo_damage") > i(a, f"p{me}_combo_damage")
            base = i(a, f"p{them}_untech")
            up = any(i(rows[j], f"p{them}_untech") > base
                     for j in range(k + 1, k + 1 + W))
            if landed:
                hit += 1
                rose += int(up)
            else:
                ctrl += 1
                ctrl_rose += int(up)
    if hit < 20:
        return Check("untech_on_hit", None, f"only {hit} landed hits")
    r_hit, r_ctrl = rose / hit, ctrl_rose / max(ctrl, 1)
    return Check("untech_on_hit", r_hit > 2.0 * r_ctrl,
                 f"untech rose after {r_hit:.1%} of {hit} landed hits vs "
                 f"{r_ctrl:.1%} of {ctrl} other ticks")


# ---------------------------------------------------------------------------
# Projectiles
#
# These need their own machinery because a bullet has no identity in the
# sidecar: the slots are re-sorted every frame by distance to the player being
# shot at, so slot 0 in one row and slot 0 in the next are not the same object.
# `_live` and `_matched` rebuild that identity where it is unambiguous.
# ---------------------------------------------------------------------------
ATTACK_BUTTONS = ("a", "b", "c", "spell")


def _n_slots(row):
    k = 0
    while f"p1_pr{k}_x" in row:
        k += 1
    return k


def _live(row, p, slots):
    """The live projectiles in one row, as dicts. `proj_n` is the exact count
    and may exceed the number of slots written, so it is clamped here."""
    n = min(i(row, f"p{p}_proj_n"), slots)
    return [{"x": f(row, f"p{p}_pr{k}_x"), "y": f(row, f"p{p}_pr{k}_y"),
             "vx": f(row, f"p{p}_pr{k}_vx"), "vy": f(row, f"p{p}_pr{k}_vy"),
             "dir": i(row, f"p{p}_pr{k}_dir"), "act": i(row, f"p{p}_pr{k}_act"),
             "hb": i(row, f"p{p}_pr{k}_hb")} for k in range(n)]


def _matched(a, b, p, slots):
    """Pair up projectiles across one tick, by action id where that is unique.

    A bullet's `action` says which bullet it is, so an action that appears
    exactly once on both sides of a tick identifies the same object on both --
    no tolerance, no nearest-neighbour guess, and no way for two bullets of the
    same type to be confused for each other. Frames where that is ambiguous are
    simply not used; there are plenty that are not.
    """
    la, lb = _live(a, p, slots), _live(b, p, slots)
    ca, cb = {}, {}
    for q in la:
        ca.setdefault(q["act"], []).append(q)
    for q in lb:
        cb.setdefault(q["act"], []).append(q)
    for act, qa in ca.items():
        qb = cb.get(act)
        if len(qa) == 1 and qb and len(qb) == 1:
            yield qa[0], qb[0]


def check_proj_owner(rows, slots):
    """A player's bullets must appear after THAT player attacks.

    The walk starts at `charObj + 0x6F8`, so the one thing most likely to be
    wrong is whose list it is -- a shared list, or the two swapped, would
    produce numbers that look entirely reasonable. This settles it by asking,
    for every tick where a player's object count rises, whether THAT player had
    just pressed an attack button, and whether the OPPONENT had.

    AS A LIFT, NOT AS A RAW RATE
    ----------------------------
    Comparing the two raw rates is confounded by how button-happy each player
    is: against an opponent who attacks constantly, "the opponent had pressed
    recently" is true most of the time whoever the bullets belong to. One
    replay in nine read 44.8% against 43.9% for that reason, on data whose
    other projectile checks pass decisively.

    So each rate is divided by that same player's own unconditional press rate
    over the replay. A lift of 1.0 means the spawn told us nothing about that
    player; the owner's lift has to beat the opponent's. This is the same base
    rate correction the untech check needed, and for the same reason.
    """
    W = 30
    own = opp = tot = 0
    base = {1: 0, 2: 0}
    ticks = 0
    for k in range(W, len(rows) - 1):
        a, b = rows[k], rows[k + 1]
        if int(b["battle_frame"]) - int(a["battle_frame"]) != 1:
            continue
        win = rows[k - W:k + 1]
        pressed = {p: any(i(r, f"p{p}_{btn}") for r in win
                          for btn in ATTACK_BUTTONS) for p in (1, 2)}
        ticks += 1
        for p in (1, 2):
            base[p] += int(pressed[p])
        for me, them in ((1, 2), (2, 1)):
            if i(b, f"p{me}_proj_n") <= i(a, f"p{me}_proj_n"):
                continue
            tot += 1
            own += int(pressed[me])
            opp += int(pressed[them])
    if tot < 30 or ticks < 100:
        return Check("proj_owner", None, f"only {tot} spawn ticks")
    # Both players contribute spawns, so the pooled denominator is the mean of
    # the two per-player base rates.
    b_rate = (base[1] + base[2]) / (2 * ticks)
    if b_rate <= 0:
        return Check("proj_owner", None, "no attack presses in this replay")
    lift_own, lift_opp = (own / tot) / b_rate, (opp / tot) / b_rate
    return Check("proj_owner", lift_own > lift_opp,
                 f"lift {lift_own:.2f} for the owner vs {lift_opp:.2f} for the "
                 f"opponent over {tot} spawns (base rate {b_rate:.1%})")


def check_proj_speed(rows, slots):
    """A projectile's speed must explain the change in its position.

    Stated the same comparative way as the character check, and for the same
    reason: the residual is not zero even when the offset is right. It also
    answers a question the character check cannot -- the character's `speed` is
    facing-relative, and whether a projectile's is too is a fact about a
    different object that has to be measured, not inherited. It is: on this
    population the world frame misses by 19.3 and the facing frame by 0.220.

    THREE RESTRICTIONS, ALL LOAD-BEARING
    ------------------------------------
    The first version of this check pooled every object on the list and read
    "neither frame fits" on all three replays, on data whose offset is exact.
    What it is asking is whether 0x0F4 holds a projectile's velocity, so the
    population it asks it of has to be projectiles that are moving under it:

      * Only objects with a LIVE HITBOX -- actual attacks. The rest of the list
        is decoration, and decoration is attached: sparks and auras have their
        position written directly each tick while `speed` sits near zero, so
        they are evidence about the game's animation code and not about this
        offset. This restriction is what separates 0.011 from 0.757.
      * Only objects that are MOVING. Where dx and v are both ~0 every
        candidate frame "explains" the motion equally and adds no information.
      * Only frames the slot array did not truncate. Slots are re-sorted every
        tick, so under truncation the object in slot k is not the object that
        was in slot k, and the pairing silently compares two different bullets.
    """
    MOVING = 1.0     # game units per tick; below this there is nothing to explain
    e_world = e_face = mag = 0.0
    n = 0
    for a, b in _steps(rows):
        for p in (1, 2):
            if (i(a, f"p{p}_proj_n") > slots) or (i(b, f"p{p}_proj_n") > slots):
                continue
            for qa, qb in _matched(a, b, p, slots):
                if qa["hb"] == 0 or qb["hb"] == 0:
                    continue
                dx = qb["x"] - qa["x"]
                if abs(dx) < MOVING:
                    continue
                e_world += abs(dx - qa["vx"])
                e_face += abs(dx - qa["vx"] * qa["dir"])
                mag += abs(dx)
                n += 1
    # 40, not 100. The population is deliberately narrow -- one replay yields
    # 64 such pairs -- and a narrow population that agrees to within 1% of the
    # distance travelled is stronger evidence than a broad one that agrees to
    # within 75%. The pass criterion below is a factor-of-two margin, which 64
    # samples settle comfortably.
    if n < 40:
        return Check("proj_speed", None, f"only {n} moving matched pairs")
    return Check("proj_speed", e_face < 0.5 * e_world,
                 f"err {e_face/n:.3f} in the facing frame vs {e_world/n:.3f} "
                 f"in world, mean |dx| {mag/n:.3f} ({n} pairs)")


def check_proj_bounds(rows, slots):
    """Live bullets must be on the stage.

    Weak on its own -- it is the check a garbage pointer fails, not the one a
    subtly wrong offset fails -- but it is the one that would catch the walk
    reading a struct that is not an ObjectManager at all.
    """
    out = tot = 0
    for r in rows:
        for p in (1, 2):
            for q in _live(r, p, slots):
                tot += 1
                out += int(not (-400 <= q["x"] <= 1700
                                and -400 <= q["y"] <= 1700))
    if tot < 100:
        return Check("proj_bounds", None, f"only {tot} live bullets")
    return Check("proj_bounds", out / tot < 0.02,
                 f"{out} of {tot} live bullets are off-stage ({out/tot:.2%})")


def check_proj_hits(rows, slots):
    """A projectile with a live hitbox must be able to deal damage.

    Restricted to ticks where the owner's OWN hitbox count is zero, so melee
    cannot be the explanation: if damage lands while the only live attack the
    player has in the world is a projectile, these objects are attacks.
    Contrasted against ticks where the same player has objects out but none of
    them is live, which is the control that makes it more than "damage happens
    during fights".

    TWO EARLIER VERSIONS WERE CONFOUNDED
    ------------------------------------
    The first scanned the slots for a live hitbox and read 10.33% against
    10.34% -- no signal at all -- because under truncation a live projectile
    could be pushed out of the array by nearer scenery, so "no live projectile"
    was largely measuring the array's width. `proj_hb` is exact over the whole
    list and fixes that.

    The second still keyed on the opponent's `hitstop`, which both sides of an
    exchange enter: the defender's hitstop can equally mean the DEFENDER just
    landed something. `combo_damage` on the owner is unambiguous about who hit
    whom, and it is already verified against the opponent's health.
    """
    W = 12
    hot = hot_hit = cold = cold_hit = 0
    for k in range(len(rows) - W):
        for me in (1, 2):
            if i(rows[k], f"p{me}_hitboxes") > 0:
                continue          # melee could explain it
            if i(rows[k], f"p{me}_proj_n") == 0:
                continue
            base = i(rows[k], f"p{me}_combo_damage")
            hit = any(i(rows[j], f"p{me}_combo_damage") > base
                      for j in range(k + 1, k + 1 + W))
            if i(rows[k], f"p{me}_proj_hb") > 0:
                hot += 1
                hot_hit += int(hit)
            else:
                cold += 1
                cold_hit += int(hit)
    if hot < 30 or cold < 30:
        return Check("proj_hits", None,
                     f"only {hot} live-hitbox / {cold} inert ticks")
    return Check("proj_hits", hot_hit / hot > 1.2 * cold_hit / max(cold, 1),
                 f"damage followed {hot_hit/hot:.2%} of {hot} live-projectile "
                 f"ticks vs {cold_hit/cold:.2%} of {cold} inert ones")


def report_proj_shape(rows, slots):
    """Not a check -- the distribution the constants are sized against.

    MAX_PROJECTILES has to cover the busiest frame of twenty characters, and
    the sample that set it is small. This prints what the game actually did on
    the data at hand, so the choice stays answerable to the corpus instead of
    to the three replays that happened to be verified.
    """
    live, hb, trunc, refused, tot = [], [], 0, 0, 0
    for r in rows:
        for p in (1, 2):
            n, h, rw = (i(r, f"p{p}_proj_n"), i(r, f"p{p}_proj_hb"),
                        i(r, f"p{p}_proj_raw"))
            live.append(n)
            hb.append(h)
            tot += 1
            trunc += int(h > slots)
            # A refusal is (nothing walked, but the game reported a length).
            # Counted here, on the row, and NOT afterwards from the two lists:
            # an earlier version sorted `live` for its percentiles and then
            # zipped it against unsorted `raw`, which paired every frame with
            # some other frame's list length and invented 2012 refusals out of
            # zero.
            refused += int(n == 0 and rw > 0)
    live.sort()
    hb.sort()
    def pc(v, x):
        return v[min(int(x * len(v)), len(v) - 1)]
    return (f"  [info] projectiles     objects: p99 {pc(live,.99)}, max "
            f"{live[-1]}, mean {sum(live)/max(tot,1):.2f} | live hitboxes: "
            f"p90 {pc(hb,.9)}, p99 {pc(hb,.99)}, max {hb[-1]}, mean "
            f"{sum(hb)/max(tot,1):.2f} | over the {slots} slots on {trunc} of "
            f"{tot} frames ({trunc/max(tot,1):.3%}) | {refused} walks refused")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("csv", type=Path, nargs="+")
    args = ap.parse_args(argv)
    failed = 0
    for path in args.csv:
        opener = gzip.open if path.suffix == ".gz" else open
        with opener(path, "rt", newline="") as fh:
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
        slots = _n_slots(rows[0])
        if slots:
            checks += [check_proj_owner(rows, slots),
                       check_proj_speed(rows, slots),
                       check_proj_bounds(rows, slots),
                       check_proj_hits(rows, slots)]
        for c in checks:
            print(c.line())
        if slots:
            print(report_proj_shape(rows, slots))
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
