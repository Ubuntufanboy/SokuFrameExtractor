"""Measure capture throughput under a given renderer configuration.

Throughput has to be measured at the concurrency it will run at. A single
capture on an idle machine tells you nothing useful about a swarm, because
every setting that matters here -- llvmpipe's thread count above all -- only
costs anything once workers are competing for the same cores.

So this runs the real swarm for a fixed wall-clock window and reports frames
per second summed across workers, reading the module's own `Perf:` lines
rather than inferring anything.

    python3 -m runner.bench --workers 4 --minutes 7 \\
        --env LP_NUM_THREADS=1 --env LP_NATIVE_VECTOR_WIDTH=256
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

PERF = re.compile(r"Perf: ([0-9.]+) fps")


def worker_log(prefixes: Path, i: int, game_subdir: str) -> Path:
    return (prefixes / f"w{i}" / game_subdir / "modules" / "SokuFrameExtractor"
            / "SokuFrameExtractor.log")


def frames_written(log: Path, manifest: Path) -> int:
    """Frames captured by this worker so far, monotonically.

    The module's "encoder wrote N" counter is per *replay* and resets to zero
    at every boundary, so differencing it across a window goes negative
    whenever a worker happens to start a new replay inside that window -- the
    first version of this benchmark reported 3.0 fps total with one worker at
    -3, which is a broken measurement, not a slow renderer.

    Completed replays therefore come from the manifest (which never rewinds)
    and only the in-flight replay is read from the log.
    """
    total = 0
    try:
        for line in manifest.read_text(errors="ignore").splitlines():
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            if e.get("status") == "ok":
                total += e.get("frames") or 0
    except OSError:
        pass

    try:
        text = log.read_text(errors="ignore")
    except OSError:
        return total
    # Only the tail since the last session start belongs to the in-flight replay.
    tail = text.rsplit("Session::init", 1)[-1]
    hits = re.findall(r"encoder wrote (\d+)", tail)
    return total + (int(hits[-1]) if hits else 0)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--minutes", type=float, default=7.0)
    ap.add_argument("--warmup", type=float, default=2.5,
                    help="minutes to discard while the games reach a battle")
    ap.add_argument("--prefixes", type=Path, default=Path("/root/prefixes"))
    ap.add_argument("--game-subdir", default="drive_c/Games/Soku")
    ap.add_argument("--out", type=Path, default=Path("/root/bench_out"))
    ap.add_argument("--corpus", type=Path, default=Path("/root/corpus"))
    ap.add_argument("--prefix", type=Path, default=Path("/root/wineprefix"))
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="renderer setting to apply (repeatable)")
    ap.add_argument("--label", default="")
    args = ap.parse_args(argv)

    env = dict(os.environ)
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    subprocess.run([sys.executable, "-m", "runner.swarm", "stop",
                    "--out", str(args.out), "--workers", str(args.workers)],
                   capture_output=True)
    time.sleep(4)
    subprocess.run(["pkill", "-x", "th123.exe"], capture_output=True)
    time.sleep(3)
    if args.out.exists():
        subprocess.run(["rm", "-rf", str(args.out)])
    for i in range(args.workers):
        log = worker_log(args.prefixes, i, args.game_subdir)
        if log.exists():
            log.unlink()

    subprocess.run(
        [sys.executable, "-m", "runner.swarm", "start",
         "--workers", str(args.workers), "--cpus", "4",
         "--prefix", str(args.prefix), "--prefixes", str(args.prefixes),
         "--corpus", str(args.corpus), "--out", str(args.out),
         "--budget-gb", "5", "--timeout", "1800"],
        env=env, capture_output=True)

    time.sleep(args.warmup * 60)
    start = [frames_written(worker_log(args.prefixes, i, args.game_subdir),
                            args.out / f"w{i}" / "manifest.jsonl")
             for i in range(args.workers)]
    t0 = time.monotonic()

    time.sleep(args.minutes * 60)

    end = [frames_written(worker_log(args.prefixes, i, args.game_subdir),
                          args.out / f"w{i}" / "manifest.jsonl")
           for i in range(args.workers)]
    dt = time.monotonic() - t0

    per = [(b - a) / dt for a, b in zip(start, end)]
    total = sum(per)

    label = args.label or ",".join(args.env) or "baseline"
    print(f"{label:<44} {total:7.1f} fps total   "
          f"[{' '.join(f'{p:.0f}' for p in per)}]")

    subprocess.run([sys.executable, "-m", "runner.swarm", "stop",
                    "--out", str(args.out), "--workers", str(args.workers)],
                   capture_output=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
