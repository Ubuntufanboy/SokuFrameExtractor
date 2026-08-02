"""Launching the game under Wine + Xvfb, headless and unattended.

Everything here is stdlib-only so the runner can execute inside a slim
container without a pip install step.

Two things in this module are non-obvious and were established by testing on a
real prefix rather than read out of documentation:

1. **Audio must be silenced, but not removed.** Xvfb isolates video and not
   sound, so a headless launch blasts the game's BGM through the host speakers.
   The obvious fix -- disabling Wine's audio drivers -- backfires: DirectSound
   initialisation then fails and Soku shows a modal "DSound-Error" dialog that
   nothing dismisses headlessly, so the game never renders a frame. Instead the
   drivers stay loadable and ALSA is pointed at a null device
   (``asound-null.conf``). Note override names must be the *base* names
   (``winepulse``), not module filenames (``winepulse.drv``) -- the latter form
   parses fine and silently does nothing.

2. **Xvfb rejects depth 32.** ``-screen 0 640x480x32`` dies with "Couldn't add
   screen 0". Depth 24 works and still yields llvmpipe GL 4.5 compatibility,
   far more than the extractor's GL 2.1-era PBO path needs.
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import throttle

# The game renders at a fixed 640x480. The X screen is made slightly larger so
# the window is never clipped by a stray WM decoration or centring offset.
SCREEN_W, SCREEN_H, SCREEN_DEPTH = 800, 600, 24

# The English release. The game folder also contains th123.exe, which is the
# original Japanese build -- do NOT launch that one; we collect on English.
# Launching th123e.exe (with th123e.dll alongside it) starts the translated
# game, which then appears in the process table as "th123.exe". That mismatch
# matters for teardown; see run_game().
GAME_EXE = "th123e.exe"

# Matches every Soku process regardless of which executable started it, so
# teardown catches both th123e.exe and the th123.exe it runs as.
_GAME_PROC_PATTERN = r"th123"

# Silence audio by giving Wine a *working* ALSA device that discards
# everything, not by removing its audio drivers.
#
# Disabling the drivers outright (winealsa=d, winepulse=d, ...) does silence
# the game, but DirectSound initialisation then FAILS and Soku puts up a modal
# "DSound-Error" dialog. Headless nobody dismisses it, so the game never
# renders, wglSwapBuffers is never called, and the capture FSM waits forever in
# WAIT_TITLE with no error anywhere. Silent *and* functional is what we need.
#
# winepulse is still disabled so Wine goes to ALSA, which we then point at the
# null config below.
_AUDIO_MUTE = ["winepulse=d"]

# Shipped next to this module; see the file for the full rationale.
_ASOUND_NULL_CONF = Path(__file__).with_name("asound-null.conf")

# The SWRSToys mod loader IS the d3d9.dll shipped in the game folder, and Wine
# will not prefer a local DLL over its own builtin unless told to. "n,b" means
# native first, builtin as fallback -- without it Wine silently loads its own
# d3d9, the loader never runs, and no module is ever initialised. The symptom
# is indistinguishable from a broken module: the game starts, renders fine, and
# writes no log at all.
_LOAD_MODLOADER = "d3d9=n,b"

# The inverse: force Wine's builtin d3d9 so the mod loader is bypassed and the
# game runs vanilla. Used for engine-level smoke tests, never for capture.
_BYPASS_MODLOADER = "d3d9=b"


class WineError(RuntimeError):
    pass


# Each concurrent worker reserves its own block of display numbers, set from
# SFE_DISPLAY_BASE. Probing for a free display is inherently racy: ten workers
# starting at once all see :90 unused and all try to claim it, and the losers
# fail in a way that looks like "Xvfb did not come up". Partitioning the range
# by worker removes the race instead of narrowing it.
DISPLAY_RANGE = 10


def _free_display(start: int | None = None, end: int | None = None) -> int:
    """Find an X display number nobody is using, within this worker's block.

    Checks both the abstract socket path and the lock file: a stale
    /tmp/.X<n>-lock from a killed Xvfb would otherwise make us think a display
    is taken forever.
    """
    if start is None:
        start = int(os.environ.get("SFE_DISPLAY_BASE", "90"))
    if end is None:
        end = start + DISPLAY_RANGE
    for n in range(start, end):
        if Path(f"/tmp/.X11-unix/X{n}").exists():
            continue
        if Path(f"/tmp/.X{n}-lock").exists():
            continue
        return n
    raise WineError(f"no free X display in range {start}..{end}")


@dataclass
class Xvfb:
    """A headless X server, scoped to a `with` block."""

    display: int = field(default_factory=_free_display)
    _proc: subprocess.Popen | None = field(default=None, repr=False)

    _wm: subprocess.Popen | None = field(default=None, repr=False)

    def __enter__(self) -> "Xvfb":
        if not shutil.which("Xvfb"):
            raise WineError("Xvfb not found on PATH")
        self._proc = subprocess.Popen(
            [
                "Xvfb",
                f":{self.display}",
                "-screen", "0", f"{SCREEN_W}x{SCREEN_H}x{SCREEN_DEPTH}",
                "-nolisten", "tcp",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        # Wait for the socket to appear rather than sleeping a fixed amount.
        sock = Path(f"/tmp/.X11-unix/X{self.display}")
        for _ in range(100):
            if sock.exists():
                self._start_wm()
                return self
            if self._proc.poll() is not None:
                err = (self._proc.stderr.read() or b"").decode(errors="replace")
                raise WineError(
                    f"Xvfb exited immediately (depth {SCREEN_DEPTH}): {err.strip()}"
                )
            time.sleep(0.05)
        raise WineError(f"Xvfb :{self.display} did not come up within 5s")

    def _start_wm(self) -> None:
        """Run a window manager on the new display.

        Bare Xvfb has no window manager, so no window ever receives keyboard
        focus -- and SendInput delivers to the focused window. Without this the
        game renders its title screen forever while every synthesised keypress
        is discarded, which looks exactly like a broken extractor.

        A WM is also the difference between this environment and a normal
        desktop, where the same code path worked.
        """
        wm = shutil.which("openbox") or shutil.which("twm")
        if not wm:
            return
        self._wm = subprocess.Popen(
            [wm],
            env=dict(os.environ, DISPLAY=f":{self.display}"),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(1.0)   # let it claim the root window before the game maps

    def __exit__(self, *exc) -> None:
        for proc in (self._wm, self._proc):
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()


# -------------------------------------------------------------------------
# Software rasteriser tuning
# -------------------------------------------------------------------------
# The game is D3D9 -> wined3d -> OpenGL -> llvmpipe, so every pixel is drawn
# on the CPU and llvmpipe's settings are the game's frame rate.
#
# Nothing was set before, which left two costs on the table:
#
# * **Thread count.** llvmpipe sizes its rasteriser pool from the host CPU
#   count. Inside a container that means it saw 48 and spawned ~16 threads
#   *per game*; four concurrent games put ~64 rasteriser threads on a 5.76
#   core quota. llvmpipe's workers spin while waiting for tiles, so an
#   oversubscribed pool does not merely queue -- it burns quota spinning, and
#   the cgroup was throttling 74% of scheduling periods.
#
# * **Vector width.** llvmpipe picks its SIMD width conservatively. This host
#   has AVX2 and AVX-512; 256-bit is the sweet spot in practice, because the
#   512-bit paths downclock and llvmpipe's kernels are not wide enough to pay
#   that back.
#
# Both are overridable from the environment so they can be benchmarked
# without editing code.
def mesa_env() -> dict[str, str]:
    """llvmpipe settings, sized for the CPU budget we actually have."""
    env = {
        # Force software rendering explicitly rather than relying on there
        # being no GPU to fall back from.
        "LIBGL_ALWAYS_SOFTWARE": "1",
        "GALLIUM_DRIVER": "llvmpipe",
        "MESA_NO_ERROR": "1",          # skip GL error bookkeeping we never read
        "LP_NATIVE_VECTOR_WIDTH": "256",
    }
    threads = os.environ.get("SFE_LP_NUM_THREADS")
    if threads is None:
        # Split the real budget across the workers sharing it, so the pool
        # matches the cores available rather than the cores visible.
        avail = throttle.available_cpus()
        workers = max(1, int(os.environ.get("SFE_WORKERS", "1")))
        threads = str(max(1, avail // workers))
    env["LP_NUM_THREADS"] = threads

    # Explicit overrides win, so a benchmark can set any of these directly.
    for k in ("LP_NUM_THREADS", "LP_NATIVE_VECTOR_WIDTH", "GALLIUM_DRIVER",
              "LIBGL_ALWAYS_SOFTWARE", "MESA_NO_ERROR"):
        if k in os.environ:
            env[k] = os.environ[k]
    return env


def wine_env(
    prefix: Path,
    display: int,
    *,
    mute_audio: bool = True,
    bypass_modloader: bool = False,
    extra_overrides: tuple[str, ...] = (),
) -> dict[str, str]:
    """Build the environment for a headless Wine launch.

    `mute_audio` defaults to True on purpose: a silent failure to render is
    recoverable, a surprise blast of game music through someone's speakers is
    not.
    """
    overrides: list[str] = []
    overrides.append(_BYPASS_MODLOADER if bypass_modloader else _LOAD_MODLOADER)
    if mute_audio:
        overrides.extend(_AUDIO_MUTE)
    overrides.extend(extra_overrides)

    env = dict(os.environ)
    env.update(
        {
            "DISPLAY": f":{display}",
            "WINEPREFIX": str(prefix),
            "WINEDEBUG": "-all",
            "WINEDLLOVERRIDES": ";".join(overrides),
        }
    )
    env.update(mesa_env())
    if mute_audio:
        env["PULSE_SERVER"] = "/nonexistent-sfe"
        if _ASOUND_NULL_CONF.exists():
            # ALSA reads its configuration from here; pointing it at a config
            # whose default PCM is `null` makes every device open succeed and
            # produce no sound.
            env["ALSA_CONFIG_PATH"] = str(_ASOUND_NULL_CONF)
    return env


def run_game(
    game_dir: Path,
    env: dict[str, str],
    *,
    timeout_s: float,
    log_path: Path | None = None,
    n_cpus: int = 0,
) -> tuple[int, str]:
    """Run the game to completion, or tear it down at `timeout_s`.

    Returns (returncode, reason) where reason is "exited" or "timeout"; the
    caller turns that into a manifest status.

    IMPORTANT -- what we launch is not what we have to kill. We launch
    ``th123e.exe`` (the English release), but it brings the game up under
    wineserver where it shows in the process table as ``th123.exe`` -- the same
    name as the untranslated Japanese executable sitting next to it. So:

      * ``proc.wait()`` returning does NOT mean the game is gone, and
      * killing ``proc``'s process group does NOT reap the game.

    Getting this wrong strands a fully-running game: it keeps rendering, keeps
    holding the audio device, and keeps the capture FIFO open, so the next
    replay's FFmpeg blocks forever on a pipe that never sees EOF. The reliable
    teardown is ``wineserver -k``, which kills everything in the prefix.
    """
    exe = game_dir / GAME_EXE
    if not exe.exists():
        raise WineError(f"game executable not found: {exe}")

    # The CPU budget has to be applied to the launcher: wineserver inherits the
    # affinity mask and nice level, so every process the game spawns stays
    # inside the same budget. Applying it later, to the game process, would be
    # too late -- llvmpipe has already sized its thread pool from the mask it
    # saw at startup.
    argv = throttle.wrap(["wine", GAME_EXE], n_cpus=n_cpus)

    log = open(log_path, "wb") if log_path else subprocess.DEVNULL
    try:
        proc = subprocess.Popen(
            argv,
            cwd=str(game_dir),
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        deadline = time.monotonic() + timeout_s
        try:
            proc.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            kill_prefix(Path(env["WINEPREFIX"]))
            _kill_group(proc)
            return -1, "timeout"

        # The launcher is done; wait for the game it spawned.
        if _await_prefix_idle(env, deadline):
            return 0, "exited"
        kill_prefix(Path(env["WINEPREFIX"]))
        _kill_group(proc)
        return -1, "timeout"
    finally:
        if log is not subprocess.DEVNULL:
            log.close()


def _await_prefix_idle(env: dict[str, str], deadline: float) -> bool:
    """Poll until no game process remains in *this* prefix, or `deadline` passes."""
    prefix = env["WINEPREFIX"]
    while time.monotonic() < deadline:
        if not _game_running(prefix):
            return True
        time.sleep(1.0)
    return False


def _game_running(prefix: str) -> bool:
    """True if a Soku process belonging to `prefix` is alive.

    Two things here are deliberate, and the previous version got both wrong by
    shelling out to ``pgrep -f th123``:

    * **Match the executable, not the command line.** ``pgrep -f`` matches any
      process whose *full command line* contains the pattern, which includes
      the monitoring shell that happens to be running ``pgrep -x th123.exe``.
      One stray shell made this return true forever, so every worker waited out
      its entire timeout after a capture that had already succeeded. ``comm``
      is the executable's own name and cannot be spoofed by an argument.

    * **Scope to the prefix.** The check was machine-wide, so with four workers
      each one waited for *every* other worker's game to exit too. Since the
      others immediately start their next replay, that is never. It serialised
      the swarm into something slower than a single job, silently.

    The prefix is read from the process's own environment, which is where wine
    put it, so this cannot drift from what the game was actually launched with.
    """
    return bool(_game_pids(prefix))


def _game_pids(prefix: str) -> list[int]:
    """Pids of Soku processes launched into `prefix`."""
    wanted = {prefix, os.path.realpath(prefix)}
    found: list[int] = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/comm", "rb") as fh:
                if not fh.read().strip().startswith(b"th123"):
                    continue
            with open(f"/proc/{entry}/environ", "rb") as fh:
                environ = fh.read()
        except OSError:
            continue          # exited, or not ours to read
        for var in environ.split(b"\0"):
            if var.startswith(b"WINEPREFIX="):
                if var[len(b"WINEPREFIX="):].decode(errors="replace") in wanted:
                    found.append(int(entry))
                break
    return found


def kill_prefix(prefix: Path) -> None:
    """Kill every process in a Wine prefix. The only teardown that reliably works."""
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG="-all")
    try:
        subprocess.run(
            ["wineserver", "-k"],
            env=env,
            timeout=20,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    # Belt and braces: wineserver -k occasionally leaves the game behind if it
    # is wedged in a driver call.
    #
    # Both the check and the kill are scoped to this prefix. A bare
    # `pkill -9 -f th123` would take down every *other* worker's game too --
    # and, because -f matches command lines, any shell that merely mentions
    # the name.
    for _ in range(3):
        stuck = _game_pids(str(prefix))
        if not stuck:
            return
        for pid in stuck:
            try:
                os.kill(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        time.sleep(1.0)


def _kill_group(proc: subprocess.Popen) -> None:
    """SIGTERM then SIGKILL the launcher's own process group."""
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(os.getpgid(proc.pid), sig)
        except (ProcessLookupError, PermissionError):
            return
        try:
            proc.wait(timeout=10)
            return
        except subprocess.TimeoutExpired:
            continue


def clear_crash_sentinel(game_dir: Path) -> bool:
    """Undo SWRSToys' auto-disable of a module after a load-time crash.

    SWRSToys writes the module it is about to load into ``currentModule.txt``
    and clears the file once loading succeeds. If the process dies in between,
    the stale entry survives, and on the next launch the loader disables that
    module and puts up a MODAL DIALOG:

        "SokuFrameExtractor.dll has been disabled because the game crashed
         while loading it last time."

    Headless, nobody clicks OK. The game sits at the dialog forever, produces
    no capture and no status file, and every subsequent launch does the same --
    so one crash silently converts an unattended run into a series of
    identical timeouts. Worse, the failure looks nothing like its cause: the
    logs are empty because the module never ran.

    Clearing the sentinel before each launch makes a crash cost one replay
    instead of the remainder of the corpus. Returns True if it cleared
    something.
    """
    sentinel = game_dir / "currentModule.txt"
    cleared = False
    if sentinel.exists() and sentinel.stat().st_size > 0:
        sentinel.write_text("")
        cleared = True

    # The loader also flips the module off in its JSON settings; put it back.
    settings = game_dir / "ModLoaderSettings.json"
    if settings.exists():
        try:
            data = json.loads(settings.read_text())
            modules = data.get("modules", {})
            for key, val in modules.items():
                if "SokuFrameExtractor" in key and not val.get("enabled", False):
                    val["enabled"] = True
                    cleared = True
            if cleared:
                settings.write_text(json.dumps(data, indent=1))
        except (json.JSONDecodeError, OSError):
            pass

    return cleared


def wineserver_wait(prefix: Path, timeout_s: float = 30.0) -> None:
    """Block until the prefix's wineserver has shut down.

    Without this, back-to-back replays race: the next launch reuses a
    wineserver that is still tearing down the previous game, which shows up as
    sporadic startup hangs.
    """
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG="-all")
    try:
        subprocess.run(
            ["wineserver", "-w"],
            env=env,
            timeout=timeout_s,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
