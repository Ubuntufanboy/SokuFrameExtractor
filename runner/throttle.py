"""CPU budgeting, so collection can run on a machine somebody is using.

Capture is embarrassingly parallel and will happily consume every core: the
game renders through llvmpipe (software rasterisation, multi-threaded), and
x264 spawns a thread per core beside it. On a 4-core laptop an unconstrained
run drives load average past 10 and makes the desktop unusable.

Two mechanisms, because they do different jobs:

* ``nice`` lowers priority, so the scheduler prefers interactive work. This is
  what keeps a desktop responsive, and it costs the run almost nothing when the
  machine is otherwise idle.
* ``taskset`` caps how many cores the run may touch at all. ``nice`` alone
  still lets a run saturate every core, which pins clocks high and cooks a
  laptop even when latency is fine.

Defaults are deliberately conservative: half the cores, lowest priority. A
dedicated collection box should pass ``--cpus 0`` (meaning "all") explicitly.
"""

from __future__ import annotations

import os
import shutil

# Lowest priority. The run is throughput work with no deadline; anything the
# human does should preempt it.
DEFAULT_NICE = 19


def default_cpu_count() -> int:
    """Half the machine's cores, at least 1, at most 4.

    The cap matters on big machines: past ~4 cores a single capture is bound by
    the game's own render thread, so extra cores buy nothing and just add heat.
    Scale out with more concurrent jobs instead.
    """
    total = os.cpu_count() or 2
    return max(1, min(4, total // 2))


def cpu_list(n_cpus: int) -> str:
    """Pick which cores to pin to, as a taskset/cpuset list.

    Takes the *highest*-numbered cores. On most desktop schedulers, interactive
    processes and IRQ handlers gravitate toward the low-numbered ones, so
    starting from the top leaves the user's foreground work least disturbed.
    """
    total = os.cpu_count() or 2
    n = max(1, min(n_cpus, total))
    return ",".join(str(c) for c in range(total - n, total))


def wrap(argv: list[str], *, n_cpus: int, nice: int = DEFAULT_NICE) -> list[str]:
    """Prefix `argv` with nice/taskset according to the budget.

    `n_cpus <= 0` means "no limit" and returns argv unchanged.
    """
    if n_cpus <= 0:
        return list(argv)

    prefix: list[str] = []
    if nice and shutil.which("nice"):
        prefix += ["nice", "-n", str(nice)]
    if shutil.which("taskset"):
        prefix += ["taskset", "-c", cpu_list(n_cpus)]
    return prefix + list(argv)


def describe(n_cpus: int, nice: int = DEFAULT_NICE) -> str:
    if n_cpus <= 0:
        return "unlimited CPU"
    return f"{n_cpus} core(s) [{cpu_list(n_cpus)}], nice {nice}"
