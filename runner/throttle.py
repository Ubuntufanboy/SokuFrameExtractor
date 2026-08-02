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


def cgroup_cpu_quota() -> float | None:
    """CPUs this container may actually use, or None if unrestricted.

    `os.cpu_count()` reports the *host's* CPUs, which inside a container can be
    a wild overestimate. The GPU box advertises 48 and is capped at 5.76 by
    cgroup quota, so a swarm sized from `nproc` oversubscribed the real budget
    seven-fold: ten workers pinned to ten disjoint four-core blocks, all
    sharing 5.76 cores, each capturing at 5 fps instead of 95.

    The symptom is deeply misleading -- `vmstat` shows 78 runnable tasks and
    80% idle at the same time, because the idle is the host's and the runnable
    queue is ours. Read the quota instead of inferring it.
    """
    # cgroup v2
    try:
        raw = open("/sys/fs/cgroup/cpu.max").read().split()
        if raw and raw[0] != "max":
            return int(raw[0]) / int(raw[1])
    except (OSError, ValueError, IndexError):
        pass
    # cgroup v1
    try:
        quota = int(open("/sys/fs/cgroup/cpu/cpu.cfs_quota_us").read())
        period = int(open("/sys/fs/cgroup/cpu/cpu.cfs_period_us").read())
        if quota > 0 and period > 0:
            return quota / period
    except (OSError, ValueError):
        pass
    return None


def available_cpus() -> int:
    """Cores we can really use: the smaller of the host count and the quota."""
    total = os.cpu_count() or 2
    quota = cgroup_cpu_quota()
    if quota is not None:
        total = max(1, min(total, int(quota)))
    return total


def default_cpu_count() -> int:
    """Half the available cores, at least 1, at most 4.

    The cap matters on big machines: past ~4 cores a single capture is bound by
    the game's own render thread, so extra cores buy nothing and just add heat.
    Scale out with more concurrent jobs instead -- but only as far as
    `available_cpus()` allows, not as far as `nproc` claims.
    """
    return max(1, min(4, available_cpus() // 2))


def cpu_block() -> int:
    """Which block of cores this process should take, from SFE_CPU_BLOCK.

    Set by runner/swarm.py, one distinct value per worker.
    """
    try:
        return max(0, int(os.environ.get("SFE_CPU_BLOCK", "0")))
    except ValueError:
        return 0


def cpu_list(n_cpus: int, block: int | None = None) -> str:
    """Pick which cores to pin to, as a taskset/cpuset list.

    Blocks are taken from the *highest*-numbered cores downward. On a desktop,
    interactive processes and IRQ handlers gravitate toward the low-numbered
    cores, so starting from the top leaves foreground work least disturbed.

    `block` selects a disjoint group, which is what makes concurrent workers
    work at all. Without it every worker got the same top `n_cpus` cores: ten
    collectors and their ten FFmpegs all pinned to cores 44-47 of a 48-thread
    machine. Capture ran at 20 fps instead of 95 and the swarm was *slower*
    than a single job -- it looked like contention for the game's renderer,
    and it was twenty processes fighting over four cores.

    Blocks wrap if more are requested than fit, which oversubscribes rather
    than failing: a slow run beats a run that will not start.
    """
    total = available_cpus()
    n = max(1, min(n_cpus, total))
    if block is None:
        block = cpu_block()
    start = total - n * (block + 1)
    return ",".join(str((start + k) % total) for k in range(n))


def wrap(argv: list[str], *, n_cpus: int, nice: int = DEFAULT_NICE) -> list[str]:
    """Prefix `argv` with nice/taskset according to the budget.

    `n_cpus <= 0` means "no limit" and returns argv unchanged.
    """
    if n_cpus <= 0:
        return list(argv)

    prefix: list[str] = []
    if nice and shutil.which("nice"):
        prefix += ["nice", "-n", str(nice)]

    # Do not pin inside a quota-limited container.
    #
    # taskset exists here to stop a run from monopolising a machine somebody
    # is using. A cgroup quota already does that, and does it better: it caps
    # total CPU while leaving the scheduler free to place threads wherever
    # there is room. Adding taskset on top does not cap anything further -- it
    # only forbids the scheduler from using idle cores, so workers queue
    # behind each other for their assigned block while the rest of the machine
    # sits available.
    #
    # Measured on the 5.76-core GPU box: three workers pinned to two cores
    # each ran 48/52/5 fps, the third starved because its block wrapped onto a
    # neighbour's. Unpinned, the same three share the quota evenly.
    if shutil.which("taskset") and cgroup_cpu_quota() is None:
        prefix += ["taskset", "-c", cpu_list(n_cpus)]
    return prefix + list(argv)


def describe(n_cpus: int, nice: int = DEFAULT_NICE) -> str:
    if n_cpus <= 0:
        return "unlimited CPU"
    q = cgroup_cpu_quota()
    if q is not None:
        return (f"nice {nice}, unpinned "
                f"(cgroup quota {q:.2f} cores is the real limit)")
    return (f"{n_cpus} core(s) [{cpu_list(n_cpus)}] "
            f"block {cpu_block()}, nice {nice}")
