"""Run N collectors in parallel, each on its own Wine prefix.

    python3 -m runner.swarm start  --workers 10 --corpus corpus --out out --budget-gb 55
    python3 -m runner.swarm status --out out
    python3 -m runner.swarm stop   --out out

WHY PROCESSES AND NOT CONTAINERS
--------------------------------
Containers would be the natural unit, and `docker/collect.Dockerfile` exists
for hosts that have Docker. The GPU box does not: it *is* an unprivileged
container already, with no Docker daemon and no way to nest one.

What containers were actually buying here is one thing -- a private Wine
prefix per job -- because a prefix has exactly one wineserver, and two
collectors sharing one fight over the staged replay directory and the status
file while looking perfectly healthy. `collect.py` already enforces that with
an exclusive flock on the prefix. So a worker here is a process with its own
prefix, its own X display block and its own output tree, which gives the same
guarantee the container was there to provide.

PREFIX CLONING
--------------
A prefix is ~2.6 GB, and 2.1 GB of that is five read-only `.dat` archives.
Copying them ten times would cost 21 GB of a 75 GB disk -- a third of the
budget, spent on identical bytes. They are hardlinked instead, so ten prefixes
cost ~6 GB rather than ~26 GB.

Only the `.dat` files are shared, deliberately. Hardlinking the whole tree
would be cheaper still, but Wine and the game both write into the prefix
(registry hives, `configex123.ini`, the module's `.ini` and log, SWRSToys'
`currentModule.txt`), and an in-place write through a hardlink corrupts every
worker at once. The `.dat` archives are the only files large enough to matter
*and* never written.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

from . import throttle

# Files big enough to be worth sharing and known never to be written.
SHARED_SUFFIXES = (".dat",)
MIN_SHARE_BYTES = 8 * 1024 * 1024

# Each worker gets its own block of X display numbers; see wine.DISPLAY_RANGE.
DISPLAY_BLOCK = 10
DISPLAY_BASE = 90


def _clone_tree(src: Path, dst: Path, *, share: bool, log=print) -> tuple[int, int]:
    """Copy a tree, preserving symlinks. Returns (bytes copied, bytes shared).

    Symlinks are recreated as symlinks, never followed. That is not a detail:
    a Wine prefix contains `dosdevices/z: -> /`, so following links would try
    to copy the entire filesystem into the clone, and `dosdevices/c:` would
    stop being a drive mapping and become a second copy of drive_c.
    """
    copied = shared = 0
    for root, dirs, files in os.walk(src, followlinks=False):
        rel = Path(root).relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)

        # os.walk lists symlinked directories in `dirs` but does not descend
        # into them. Recreate them here and drop them from the descent list.
        for name in list(dirs):
            s_dir = Path(root) / name
            if s_dir.is_symlink():
                d_dir = dst / rel / name
                if not d_dir.exists(follow_symlinks=False):
                    os.symlink(os.readlink(s_dir), d_dir)
                dirs.remove(name)

        for name in files:
            s_file = Path(root) / name
            d_file = dst / rel / name
            if s_file.is_symlink():
                if not d_file.exists(follow_symlinks=False):
                    os.symlink(os.readlink(s_file), d_file)
                continue
            try:
                st = s_file.stat()
            except OSError:
                continue
            if (share and s_file.suffix.lower() in SHARED_SUFFIXES
                    and st.st_size >= MIN_SHARE_BYTES):
                try:
                    os.link(s_file, d_file)
                    shared += st.st_size
                    continue
                except OSError:
                    pass          # cross-device or unsupported: copy instead
            try:
                shutil.copy2(s_file, d_file)
                copied += st.st_size
            except OSError as e:
                log(f"  warning: {s_file}: {e}")
    return copied, shared


def clone_prefix(src: Path, dst: Path, *, game_subdir: str, log=print) -> None:
    """Clone a Wine prefix, giving the worker its own game directory.

    The game lives at `<prefix>/<game_subdir>`, which on this host is a
    *symlink* to a shared install. That cannot be shared between workers:
    `collect.py` writes a per-job `SokuFrameExtractor.ini` into the module
    directory, the DLL writes its log beside it, and SWRSToys writes
    `currentModule.txt` and crash dumps into the game root. Ten workers
    sharing that would interleave their configuration and quietly capture the
    wrong replay.

    So the game is resolved through the symlink and cloned as a real directory
    inside each worker's prefix, with the big read-only `.dat` archives
    hardlinked back to the original.
    """
    if (dst / game_subdir / "th123.exe").exists():
        log(f"  {dst.name}: already present, reusing")
        return

    game_src = (src / game_subdir).resolve()
    if not game_src.exists():
        raise SystemExit(f"game directory not found: {src / game_subdir}")

    # 1. The prefix itself, minus the game.
    copied, _ = _clone_tree(src, dst, share=False, log=log)

    # 2. The game, as a real directory this time.
    game_dst = dst / game_subdir
    if game_dst.is_symlink():
        game_dst.unlink()
    game_dst.mkdir(parents=True, exist_ok=True)
    g_copied, g_shared = _clone_tree(game_src, game_dst, share=True, log=log)

    log(f"  {dst.name}: {(copied + g_copied) / 1e6:.0f} MB copied, "
        f"{g_shared / 1e6:.0f} MB hardlinked")


def worker_paths(out: Path, i: int) -> dict:
    return {
        "out": out / f"w{i}",
        "log": out / f"w{i}.log",
        "pid": out / f"w{i}.pid",
    }


def cmd_start(args) -> int:
    src_prefix = args.prefix.resolve()
    if not src_prefix.exists():
        print(f"error: prefix not found: {src_prefix}", file=sys.stderr)
        return 2
    corpus = args.corpus.resolve()
    if not any(corpus.glob("*.rep")):
        print(f"error: no .rep files in {corpus}", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    args.prefixes.mkdir(parents=True, exist_ok=True)

    n_rep = len(list(corpus.glob("*.rep")))
    per_worker_gb = args.budget_gb / args.workers

    # Size against the cgroup quota, not nproc. Getting this wrong is not a
    # gentle degradation: ten workers on a 5.76-core quota ran at 5 fps each,
    # i.e. half the throughput of a *single* worker, because every one of them
    # was paying full per-frame overhead for a twentieth of a core.
    avail = throttle.available_cpus()
    quota = throttle.cgroup_cpu_quota()
    requested = args.workers * args.cpus
    print(f"corpus   : {n_rep} replays in {corpus}")
    print(f"cpu      : {avail} usable"
          + (f" (cgroup quota {quota:.2f} of {os.cpu_count()} host cores)"
             if quota is not None else f" (no cgroup quota)"))
    print(f"workers  : {args.workers} x {args.cpus} cores = {requested} requested")
    if requested > avail:
        best = max(1, avail // max(1, args.cpus))
        print(f"           WARNING: that is {requested / avail:.1f}x the real "
              f"budget. Oversubscribing costs throughput outright, it does not "
              f"just share it out.")
        print(f"           Consider --workers {best} at --cpus {args.cpus}, "
              f"or --cpus {max(1, avail // args.workers)} at "
              f"--workers {args.workers}.")
    print(f"budget   : {args.budget_gb:.0f} GB total "
          f"({per_worker_gb:.1f} GB per worker)")
    print(f"prefixes : {args.prefixes}")
    print()

    print("cloning prefixes (hardlinking .dat archives)...")
    for i in range(args.workers):
        clone_prefix(src_prefix, args.prefixes / f"w{i}",
                     game_subdir=args.game_subdir)
    print()

    launched = []
    for i in range(args.workers):
        p = worker_paths(args.out, i)
        p["out"].mkdir(parents=True, exist_ok=True)

        argv = [
            sys.executable, "-m", "runner.collect",
            "--prefix", str(args.prefixes / f"w{i}"),
            "--game-subdir", args.game_subdir,
            "--replay-dir", str(corpus),
            "--out", str(p["out"]),
            "--shard", f"{i}/{args.workers}",
            "--cpus", str(args.cpus),
            "--max-output-gb", f"{per_worker_gb:.3f}",
            "--timeout", str(args.timeout),
            "--crf", str(args.crf),
            "--resume",
        ]

        env = dict(
            os.environ,
            # Partition the X display space so ten simultaneous starts cannot
            # race for the same display number.
            SFE_DISPLAY_BASE=str(DISPLAY_BASE + i * DISPLAY_BLOCK),
            # ...and the cores. Without this every worker pinned to the same
            # top `--cpus` cores: ten collectors and ten FFmpegs on four cores
            # of a 48-thread box, capturing at 20 fps instead of 95. The swarm
            # was slower than a single job. See throttle.cpu_list.
            SFE_CPU_BLOCK=str(i),
        )

        log = open(p["log"], "ab")
        log.write(f"\n=== swarm start {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n"
                  .encode())
        log.flush()

        proc = subprocess.Popen(
            argv,
            stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
            env=env,
            # Its own process group, so `stop` can signal the whole worker --
            # collector, Xvfb, wine and ffmpeg -- with one kill, and so a
            # Ctrl-C here does not take the swarm down by accident.
            start_new_session=True,
            cwd=str(Path(__file__).resolve().parent.parent),
        )
        p["pid"].write_text(f"{proc.pid}\n")
        launched.append(proc.pid)
        print(f"  w{i}: pid {proc.pid}, display "
              f":{DISPLAY_BASE + i * DISPLAY_BLOCK}+, log {p['log'].name}")

    print(f"\n{len(launched)} workers running, detached.")
    print(f"  python3 -m runner.swarm status --out {args.out}")
    print(f"  python3 -m runner.swarm stop   --out {args.out}")
    return 0


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, PermissionError):
        return False


def _dir_bytes(p: Path) -> int:
    total = 0
    for f in p.rglob("*"):
        try:
            if f.is_file():
                total += f.stat().st_size
        except OSError:
            continue
    return total


def cmd_status(args) -> int:
    rows = []
    total_ok = total_fail = 0
    total_bytes = 0
    total_frames = 0

    for i in range(args.workers):
        p = worker_paths(args.out, i)
        if not p["pid"].exists():
            continue
        pid = int(p["pid"].read_text().strip())
        alive = _alive(pid)

        ok = fail = 0
        frames = 0
        mf = p["out"] / "manifest.jsonl"
        if mf.exists():
            for line in mf.read_text(errors="ignore").splitlines():
                try:
                    e = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if e.get("status") == "ok":
                    ok += 1
                    frames += e.get("frames") or 0
                else:
                    fail += 1
        b = _dir_bytes(p["out"])
        rows.append((i, pid, "running" if alive else "stopped", ok, fail,
                     b / 1e9, frames / 60 / 3600))
        total_ok += ok
        total_fail += fail
        total_bytes += b
        total_frames += frames

    if not rows:
        print(f"no workers found under {args.out}")
        return 1

    print(f"{'w':>3} {'pid':>8} {'state':>8} {'ok':>5} {'fail':>5} "
          f"{'GB':>7} {'hours':>7}")
    for i, pid, state, ok, fail, gb, hrs in rows:
        print(f"{i:>3} {pid:>8} {state:>8} {ok:>5} {fail:>5} {gb:>7.1f} {hrs:>7.2f}")

    hours = total_frames / 60 / 3600
    print(f"\ntotal: {total_ok} ok, {total_fail} failed, "
          f"{total_bytes / 1e9:.1f} GB, {hours:.2f} hours of footage")
    if hours > 0:
        print(f"rate : {total_bytes / 1e6 / hours:.0f} MB per hour of footage")
    running = sum(1 for r in rows if r[2] == "running")
    print(f"alive: {running}/{len(rows)} workers")
    return 0


def cmd_stop(args) -> int:
    stopped = 0
    for i in range(args.workers):
        p = worker_paths(args.out, i)
        if not p["pid"].exists():
            continue
        pid = int(p["pid"].read_text().strip())
        if not _alive(pid):
            continue
        try:
            # Signal the whole process group: the collector alone would leave
            # Xvfb, wine and ffmpeg orphaned and still holding the prefix.
            os.killpg(os.getpgid(pid), signal.SIGTERM)
            stopped += 1
        except (ProcessLookupError, PermissionError) as e:
            print(f"  w{i}: {e}")
    print(f"sent SIGTERM to {stopped} worker group(s)")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--out", type=Path, default=Path("out"))
    common.add_argument("--workers", type=int, default=10)

    s = sub.add_parser("start", parents=[common])
    s.add_argument("--prefix", type=Path, required=True,
                   help="the Wine prefix to clone")
    s.add_argument("--prefixes", type=Path, default=Path("prefixes"),
                   help="where the per-worker clones live")
    s.add_argument("--corpus", type=Path, required=True,
                   help="directory of .rep files (see pipeline.fetch_replays)")
    s.add_argument("--game-subdir", default="drive_c/Games/Soku")
    s.add_argument("--budget-gb", type=float, default=55.0,
                   help="total output budget across all workers")
    s.add_argument("--cpus", type=int, default=4,
                   help="cores per worker")
    s.add_argument("--timeout", type=float, default=900.0)
    s.add_argument("--crf", type=int, default=26)
    s.set_defaults(func=cmd_start)

    sub.add_parser("status", parents=[common]).set_defaults(func=cmd_status)
    sub.add_parser("stop", parents=[common]).set_defaults(func=cmd_stop)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
