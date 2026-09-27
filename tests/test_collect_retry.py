"""A capture whose game quit before the module ran is retried; nothing else is.

    python -m pytest tests/test_collect_retry.py -q
"""
from __future__ import annotations

from types import SimpleNamespace as E

import pytest

from runner.collect import exited_before_module_ran


def test_the_first_launch_early_exit_is_recognised():
    assert exited_before_module_ran(E(status="failed", frames=0,
                                      reason="no status.json (game exited rc=0)"))


@pytest.mark.parametrize("entry", [
    E(status="ok", frames=10079, reason=None),
    E(status="failed", frames=0, reason="never reached the title screen"),      # the DLL decided
    E(status="failed", frames=312, reason="no status.json (game exited rc=0)"), # it got going
    E(status="timeout", frames=0, reason="no status.json (game exited rc=-9)"),
    E(status="failed", frames=0, reason=None),
])
def test_nothing_else_is_retried(entry):
    assert not exited_before_module_ran(entry)


def test_an_environment_that_will_not_come_up_is_a_recorded_failure_not_a_dead_worker(
        tmp_path, monkeypatch):
    """Amarel: "Xvfb :90 did not come up within 5s" escaped capture_one and killed the worker,
    abandoning the rest of its shard. It must come back as a failed Entry."""
    from runner import collect, encode, wine

    class DeadXvfb:
        def __enter__(self):
            raise wine.WineError("Xvfb :90 did not come up within 5s")

        def __exit__(self, *a):
            return False

    class NullEncoder:
        def __init__(self, **kw): pass
        def __enter__(self): return self
        def __exit__(self, *a): return False

    monkeypatch.setattr(wine, "Xvfb", DeadXvfb)
    monkeypatch.setattr(encode, "Encoder", NullEncoder)
    monkeypatch.setattr(wine, "clear_crash_sentinel", lambda g: False)
    killed = []
    monkeypatch.setattr(wine, "kill_prefix", lambda p: killed.append(p))
    rep = tmp_path / "5262777.rep"
    rep.write_bytes(b"not really a replay")
    (tmp_path / "module").mkdir()
    entry = collect.capture_one(
        rep, prefix=tmp_path / "prefix", game_dir=tmp_path / "game",
        module_dir=tmp_path / "module", out_root=tmp_path / "out", work=tmp_path / "work",
        n_cpus=1, timeout_s=5, vaapi=False, crf=26, square=480, verbose=False, video=False)
    assert entry.status == "failed" and "Xvfb" in entry.reason
    assert killed == [tmp_path / "prefix"]          # the prefix is still torn down
