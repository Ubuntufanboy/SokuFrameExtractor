"""Drive Soku's menus from outside the game, via the X server.

WHY THIS IS NOT DONE IN THE DLL
-------------------------------
Soku reads the keyboard through DirectInput, and Wine's dinput reads key state
from the X server rather than from Wine's synthetic Win32 input queue. Five
in-process mechanisms were tried and every one was swallowed -- the game
rendered its title screen while the scene id never moved:

    SendInput                                   (the original approach)
    SendInput + SetForegroundWindow/SetFocus
    PostMessage(WM_KEYDOWN/WM_KEYUP)
    writing KeymapManager.inKeys directly       (crashed: unverified offsets)
    all of the above with a window manager running

All of those inject on the *Windows* side of the boundary. XTEST is different
in kind: it asks the X server itself to synthesise key events, so they arrive
by the same route as a physical keyboard -- which is the route Wine's dinput
actually reads.

The window must hold focus for the events to land, which is why the runner now
starts a window manager (see wine.Xvfb._start_wm).
"""

from __future__ import annotations

import subprocess
import time
from pathlib import Path

from . import vkbd

# Linux evdev key names -- these go to a uinput virtual keyboard, not to X.
KEY_CONFIRM = vkbd.KEY_CONFIRM
KEY_UP = vkbd.KEY_UP
KEY_DOWN = vkbd.KEY_DOWN

# title -> main menu -> Watch Replay -> list -> start.
# Exactly one .rep is staged, so the list has a single entry already selected.
NAV_SEQUENCE: list[tuple[float, str]] = [
    (1.5, KEY_CONFIRM),   # title -> main menu
    (1.2, KEY_UP),        # wrap upward to "Watch Replay"
    (0.3, KEY_UP),
    (0.3, KEY_UP),
    (0.3, KEY_UP),
    (0.3, KEY_UP),
    (0.3, KEY_UP),
    (0.7, KEY_CONFIRM),   # enter Watch Replay
    (1.0, KEY_CONFIRM),   # confirm / open the replay list
    (1.0, KEY_CONFIRM),   # select the single staged entry
    (0.7, KEY_CONFIRM),   # start playback
]

WINDOW_NAME = "Touhou"


def _run(argv: list[str], env: dict[str, str]) -> subprocess.CompletedProcess:
    return subprocess.run(argv, env=env, capture_output=True, text=True)


def find_window(env: dict[str, str], timeout_s: float = 60.0) -> str | None:
    """Wait for the game window and return its X id."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        res = _run(["xdotool", "search", "--name", WINDOW_NAME], env)
        ids = [ln for ln in res.stdout.split() if ln.strip()]
        if ids:
            return ids[-1]
        time.sleep(1.0)
    return None


def focus(win: str, env: dict[str, str]) -> None:
    """Give the window focus. XTEST delivers to whatever is focused."""
    _run(["xdotool", "windowactivate", "--sync", win], env)
    _run(["xdotool", "windowfocus", "--sync", win], env)
    _run(["xdotool", "windowraise", win], env)


def send_key(key: str, win: str, env: dict[str, str]) -> None:
    """Send one keystroke through XTEST.

    Focus is re-asserted per key: a scene change can hand focus elsewhere, and
    a silently-dropped keystroke mid-sequence leaves the menu in a state the
    rest of the sequence does not expect.
    """
    focus(win, env)
    _run(["xdotool", "key", "--clearmodifiers", key], env)


def wait_for_title(dll_log: Path, timeout_s: float = 90.0, *, log=print) -> bool:
    """Block until the module reports the title screen is up.

    The game window is mapped within a couple of seconds but the title screen
    is not reachable for ~15 s under llvmpipe, and the delay varies with host
    load. Firing the sequence on window-appearance alone puts every keystroke
    into the logo animation.

    The module already logs "Title reached" once it observes the scene id, so
    gate on that rather than guessing a delay.
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            if "Title reached" in dll_log.read_text(errors="ignore"):
                return True
        except OSError:
            pass
        time.sleep(1.0)
    log(f"menu: module never reported the title screen within {timeout_s:.0f}s")
    return False


def navigate(
    env: dict[str, str],
    dll_log: Path,
    keyboard: "vkbd.VirtualKeyboard",
    *,
    log=print,
) -> bool:
    """Walk the menus to start the staged replay, via the virtual keyboard.

    `keyboard` must already be open -- it has to exist before the game starts
    so Wine's dinput enumerates it. Focus still matters, so the window is
    raised first even though the events themselves come from the kernel.
    """
    win = find_window(env)
    if not win:
        log("menu: game window never appeared")
        return False

    if not wait_for_title(dll_log, log=log):
        return False

    focus(win, env)
    log("menu: driving the game via uinput virtual keyboard")
    for delay, key in NAV_SEQUENCE:
        time.sleep(delay)
        keyboard.tap(key)
        log(f"menu: tapped {key}")
    return True
