"""Core pinning must stay inside the cores this process is allowed to use.

A Slurm job is confined to a cpuset; `taskset -c` naming a core outside it fails the launch.

    python -m pytest tests/test_throttle.py -q
"""
from __future__ import annotations

import pytest

from runner import throttle


@pytest.fixture
def cpuset(monkeypatch):
    def use(cores, quota=None):
        monkeypatch.setattr(throttle.os, "sched_getaffinity", lambda pid: set(cores))
        monkeypatch.setattr(throttle, "cgroup_cpu_quota", lambda: quota)
    return use


def ids(s):
    return [int(x) for x in s.split(",")]


def test_a_workstation_is_unchanged(cpuset):
    cpuset(range(12))
    assert throttle.cpu_list(4, block=0) == "8,9,10,11"
    assert throttle.cpu_list(4, block=1) == "4,5,6,7"


def test_a_slurm_cpuset_is_respected(cpuset):
    allowed = list(range(17, 33))                      # 16 cores somewhere on a 64-core node
    cpuset(allowed)
    blocks = [ids(throttle.cpu_list(4, block=b)) for b in range(4)]
    assert all(set(b) <= set(allowed) for b in blocks)
    assert len({c for b in blocks for c in b}) == 16   # four disjoint blocks cover the job


def test_a_scattered_cpuset_is_respected(cpuset):
    allowed = [3, 9, 10, 40, 41, 63]
    cpuset(allowed)
    for b in range(4):
        assert set(ids(throttle.cpu_list(2, block=b))) <= set(allowed)


def test_a_quota_smaller_than_the_cpuset_caps_it(cpuset):
    cpuset(range(0, 48), quota=5.76)
    assert throttle.available_cpus() == 5
    assert set(ids(throttle.cpu_list(4, block=0))) <= set(range(43, 48))
