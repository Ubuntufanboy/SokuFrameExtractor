"""Drive headless capture over a corpus of .rep files, one game process each.

Per replay:

    stage the .rep  ->  write a per-job .ini  ->  mkfifo + start FFmpeg
      ->  launch the game under Xvfb  ->  wait for status.json
      ->  validate  ->  append to the manifest  ->  tear down

Run:
    python3 -m runner.collect --prefix ~/.wine-soku --out out --limit 3

Everything is stdlib-only so this runs inside a slim container unchanged.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
from pathlib import Path

from pipeline import manifest, validate

from . import encode, throttle, wine

# Give a replay this long, wall-clock, before declaring it hung. A Soku match
# is 2-4 minutes; unthrottled capture runs it faster than real time, but
# software rendering on a busy laptop can run it slower, so the bound is
# generous. It exists to catch wedges, not to pace anything.
DEFAULT_TIMEOUT_S = 900

# Refuse to start a run that would fill the disk. Captures average ~15 MB, but
# a wedged FFmpeg writing raw BGRA can produce gigabytes.
MIN_FREE_GB = 5.0


class PrefixLock:
    """Exclusive lock on a Wine prefix, held for the whole run.

    A Wine prefix has exactly one wineserver. Two collectors pointed at the
    same prefix -- easily done by launching two containers against one
    `-v sfe-prefix:/wineprefix` volume -- each start a game that the other's
    wineserver also sees, they fight over the staged replay directory and the
    status file, and both produce garbage. Nothing about it looks like an
    error: each run just sits there timing out.

    flock is released automatically if the process dies, so a killed run does
    not strand the lock.
    """

    def __init__(self, prefix: Path):
        self.path = prefix / ".sfe-collect.lock"
        self._fh = None

    def __enter__(self) -> "PrefixLock":
        import fcntl

        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._fh = open(self.path, "w")
        try:
            fcntl.flock(self._fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self._fh.close()
            raise SystemExit(
                f"error: another collector is already using {self.path.parent}.\n"
                f"       One prefix serves one run; give each concurrent job its "
                f"own prefix volume."
            )
        self._fh.write(f"{os.getpid()}\n")
        self._fh.flush()
        return self

    def __exit__(self, *exc) -> None:
        if self._fh:
            self._fh.close()
        self.path.unlink(missing_ok=True)


def find_replays(replay_dir: Path) -> list[Path]:
    return sorted(p for p in replay_dir.rglob("*") if p.suffix.lower() == ".rep")


def free_gb(path: Path) -> float:
    st = shutil.disk_usage(path)
    return st.free / (1 << 30)


def is_tmpfs(path: Path) -> bool:
    """True if `path` lives on a RAM-backed filesystem.

    Worth checking explicitly: /tmp is tmpfs on most modern distributions, and
    an output directory there is silently backed by RAM. A full corpus is
    several GB of video, so the run does not fail with a disk error -- it eats
    the machine's memory and takes the desktop down with it.
    """
    try:
        with open("/proc/mounts") as fh:
            mounts = [ln.split() for ln in fh if len(ln.split()) >= 3]
    except OSError:
        return False

    resolved = path.resolve()
    best, best_type = Path("/"), ""
    for dev, mount, fstype, *_ in mounts:
        mp = Path(mount)
        if (resolved == mp or mp in resolved.parents) and len(mp.parts) >= len(best.parts):
            best, best_type = mp, fstype
    return best_type in ("tmpfs", "ramfs")


def write_ini(
    dest: Path, *, replay_dir: Path, output_dir: Path, fifo: Path, status: Path,
    fast_forward: bool, verbose: bool,
) -> None:
    """Write the DLL's .ini for one job.

    Paths cross into Wine, so they are written as Z:\\-rooted Windows paths --
    Wine maps Z:\\ to the Linux filesystem root.
    """
    def win(p: Path) -> str:
        return "Z:" + str(p.resolve()).replace("/", "\\")

    dest.write_text(
        "; generated per job by runner/collect.py -- do not edit\n"
        "[General]\n"
        f"ReplayDir = {win(replay_dir)}\n"
        f"OutputDir = {win(output_dir)}\n"
        f"FifoPath = {win(fifo)}\n"
        f"StatusPath = {win(status)}\n"
        f"FastForward = {1 if fast_forward else 0}\n"
        f"Verbose = {1 if verbose else 0}\n"
    )


def capture_one(
    rep: Path,
    *,
    prefix: Path,
    game_dir: Path,
    module_dir: Path,
    out_root: Path,
    work: Path,
    n_cpus: int,
    timeout_s: float,
    vaapi: bool,
    crf: int,
    verbose: bool,
) -> manifest.Entry:
    """Capture a single replay. Always returns an Entry; never raises for
    ordinary failures, because one bad replay must not end the run."""
    rid = manifest.replay_id(rep)
    out_dir = out_root / rid
    out_dir.mkdir(parents=True, exist_ok=True)

    entry = manifest.Entry(
        replay_id=rid,
        source_rep=str(rep.resolve()),
        source_sha256=manifest.sha256_file(rep),
        status="failed",
    )

    # --- stage exactly one .rep -------------------------------------------
    # This is the mechanism that makes identity exact: the game can only see
    # one replay, so "the first entry in the list" is unambiguously this file.
    staged = work / "replay"
    if staged.exists():
        shutil.rmtree(staged)
    staged.mkdir(parents=True)
    shutil.copy2(rep, staged / rep.name)

    fifo = work / "video.fifo"
    status_path = work / "status.json"
    status_path.unlink(missing_ok=True)

    write_ini(
        module_dir / "SokuFrameExtractor.ini",
        replay_dir=staged,
        output_dir=out_dir,
        fifo=fifo,
        status=status_path,
        fast_forward=True,
        verbose=verbose,
    )

    # A previous crash during module load leaves SWRSToys' sentinel set, which
    # disables our module behind a modal dialog on every subsequent launch.
    # Clearing it here bounds the damage of one crash to one replay.
    if wine.clear_crash_sentinel(game_dir):
        entry.meta["cleared_crash_sentinel"] = True

    t0 = time.monotonic()
    try:
        # FFmpeg must be attached before the game starts; see encode.py.
        with encode.Encoder(
            fifo=fifo,
            out_mp4=out_dir / "video.mp4",
            vaapi=vaapi,
            crf=crf,
            log_path=out_dir / "ffmpeg.log",
            n_cpus=n_cpus,
        ) as enc, wine.Xvfb() as xvfb:
            env = wine.wine_env(prefix, xvfb.display)

            # Menus are driven inside the game by the module, which hooks
            # checkKeyOneshot -- the function Soku itself calls to ask whether
            # a key was pressed. Nothing needs to be injected from out here,
            # which is what finally made this work headlessly: no DirectInput,
            # X server, or kernel input device is involved at all.
            # runner/menu.py and runner/vkbd.py are kept for reference; see
            # their docstrings for the six approaches that did not work.
            rc, reason = wine.run_game(
                game_dir, env,
                timeout_s=timeout_s,
                log_path=out_dir / "wine.log",
                n_cpus=n_cpus,
            )
            # The DLL closes the FIFO as it exits, which is FFmpeg's EOF.
            enc.wait(120)

        entry.elapsed_ms = int((time.monotonic() - t0) * 1000)

        if status_path.exists():
            try:
                st = json.loads(status_path.read_text())
                entry.status = st.get("status", "failed")
                entry.frames = int(st.get("frames", 0))
                entry.reason = st.get("reason")
                entry.meta["dll_replay"] = st.get("replay")
                entry.meta["dll_elapsed_ms"] = st.get("elapsed_ms")
            except (json.JSONDecodeError, ValueError) as e:
                entry.status = "failed"
                entry.reason = f"unreadable status.json: {e}"
        else:
            # No status file means the DLL never reached a terminal state:
            # a crash, or the timeout fired first.
            entry.status = "timeout" if reason == "timeout" else "failed"
            entry.reason = (
                "game timed out before writing status"
                if reason == "timeout"
                else f"no status.json (game exited rc={rc})"
            )
    finally:
        # Always tear the prefix down, even on an exception: a surviving game
        # process holds the FIFO open and wedges the *next* replay.
        wine.kill_prefix(prefix)

    # --- validate ----------------------------------------------------------
    if entry.status == "ok":
        report = validate.validate_capture(out_dir)
        entry.validation = report.stats
        entry.video = str((out_dir / "video.mp4").resolve())
        entry.csv = str((out_dir / "inputs.csv").resolve())
        if not report.ok:
            entry.status = "invalid"
            entry.reason = "; ".join(f.detail for f in report.errors)

    return entry


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Headless Soku replay capture (one game process per replay).",
    )
    ap.add_argument("--prefix", type=Path, default=Path.home() / ".wine-soku",
                    help="Wine prefix containing the game")
    ap.add_argument("--game-subdir", default="drive_c/Games/Soku",
                    help="game directory relative to the prefix")
    ap.add_argument("--replay-dir", type=Path, default=None,
                    help="corpus of .rep files (default: <game>/replay)")
    ap.add_argument("--out", type=Path, default=Path("out"), help="output root")
    ap.add_argument("--work", type=Path, default=None,
                    help="scratch dir for FIFO/staging (default: <out>/.work)")
    ap.add_argument("--limit", type=int, default=0, help="stop after N replays")
    ap.add_argument("--jobs", type=int, default=1,
                    help="concurrent replays (default 1; >1 needs one prefix each)")
    ap.add_argument("--cpus", type=int, default=throttle.default_cpu_count(),
                    help="cores each job may use; 0 = unlimited "
                         f"(default {throttle.default_cpu_count()})")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S,
                    help="per-replay wall-clock limit in seconds")
    ap.add_argument("--crf", type=int, default=23, help="H.264 quality (lower=better)")
    ap.add_argument("--vaapi", action="store_true",
                    help="use GPU encoding if available")
    ap.add_argument("--resume", action="store_true",
                    help="skip replays already recorded ok in the manifest")
    ap.add_argument("--verbose", action="store_true", help="verbose DLL logging")
    args = ap.parse_args(argv)

    game_dir = args.prefix / args.game_subdir
    module_dir = game_dir / "modules" / "SokuFrameExtractor"
    replay_dir = args.replay_dir or (game_dir / "replay")
    work = args.work or (args.out / ".work")

    for label, p in [("prefix", args.prefix), ("game dir", game_dir),
                     ("module dir", module_dir), ("replay dir", replay_dir)]:
        if not p.exists():
            print(f"error: {label} not found: {p}", file=sys.stderr)
            return 2

    if args.jobs != 1:
        # Each concurrent game needs its own Wine prefix: they share a
        # wineserver, a staged replay directory and a FIFO otherwise.
        print("error: --jobs > 1 needs one Wine prefix per job; run separate "
              "containers instead", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)

    # Sweep a stale FIFO from a previous run that was killed externally.
    # Encoder.__exit__ removes it on any normal exit, but a SIGKILL bypasses
    # that and leaves an FFmpeg blocked on a pipe nothing will ever write to --
    # it survives indefinitely, burning a process slot and holding the path.
    stale = work / "video.fifo"
    if stale.exists():
        print(f"note: removing stale FIFO {stale}")
        stale.unlink()

    if is_tmpfs(args.out):
        print(f"error: {args.out.resolve()} is on a RAM-backed filesystem "
              f"(tmpfs). Captures are several GB and would consume memory, not "
              f"disk. Pass --out somewhere on real storage.", file=sys.stderr)
        return 2

    if free_gb(args.out) < MIN_FREE_GB:
        print(f"error: only {free_gb(args.out):.1f} GB free at {args.out}; "
              f"need {MIN_FREE_GB} GB", file=sys.stderr)
        return 2

    replays = find_replays(replay_dir)
    if not replays:
        print(f"error: no .rep files under {replay_dir}", file=sys.stderr)
        return 2

    if args.resume:
        done = manifest.completed_ids(args.out)
        before = len(replays)
        replays = [r for r in replays if manifest.replay_id(r) not in done]
        print(f"resume: skipping {before - len(replays)} already captured")

    if args.limit:
        replays = replays[: args.limit]

    vaapi = args.vaapi and encode.has_vaapi()
    if args.vaapi and not vaapi:
        print("note: --vaapi requested but unavailable; using libx264")

    print(f"replays  : {len(replays)}")
    print(f"output   : {args.out.resolve()}")
    print(f"cpu      : {throttle.describe(args.cpus)}")
    print(f"encoder  : {'h264_vaapi' if vaapi else 'libx264'}")
    print(f"free disk: {free_gb(args.out):.1f} GB")
    print()

    n_ok = 0
    with PrefixLock(args.prefix):
        for i, rep in enumerate(replays, 1):
            print(f"[{i}/{len(replays)}] {rep.name}", flush=True)

            if free_gb(args.out) < MIN_FREE_GB:
                print(f"  aborting: free space below {MIN_FREE_GB} GB")
                break

            entry = capture_one(
                rep,
                prefix=args.prefix,
                game_dir=game_dir,
                module_dir=module_dir,
                out_root=args.out,
                work=work,
                n_cpus=args.cpus,
                timeout_s=args.timeout,
                vaapi=vaapi,
                crf=args.crf,
                verbose=args.verbose,
            )
            manifest.append(args.out, entry)

            if entry.status == "ok":
                n_ok += 1
                print(f"  ok: {entry.frames} frames in {entry.elapsed_ms/1000:.1f}s")
            else:
                print(f"  {entry.status}: {entry.reason}")

    print(f"\n{n_ok}/{len(replays)} captured successfully")
    return 0 if n_ok == len(replays) else 1


if __name__ == "__main__":
    raise SystemExit(main())
