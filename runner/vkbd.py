"""A virtual keyboard, created through /dev/uinput.

WHY THIS EXISTS
---------------
Soku reads the keyboard through DirectInput, and Wine's dinput reads **real
evdev devices** -- not Wine's synthetic Win32 input queue, and not the X
server's synthetic events. Everything that injects above the kernel is
therefore invisible to it. Six mechanisms were tried and every one was
swallowed, with the game rendering normally and the scene id never moving:

    SendInput                                  (Win32 side)
    SendInput + SetForegroundWindow/SetFocus   (Win32 side)
    PostMessage(WM_KEYDOWN/WM_KEYUP)           (Win32 side)
    SendInput + a window manager for focus     (Win32 side)
    xdotool / XTEST                            (X server side)
    writing KeymapManager.inKeys directly      (crashed; offsets unverified)

This one is different in kind: uinput asks the *kernel* to create an input
device, and writes to it become genuine evdev events -- the same events a
physical keyboard produces, arriving by the same path. It is the only layer
below everything dinput could be reading.

It also explains why the container was worse than the desktop rather than
merely equal: a container has no /dev/input at all, so dinput had no keyboard
to enumerate under any circumstances.

ORDERING MATTERS
----------------
Wine's dinput enumerates devices when it initialises. The virtual keyboard has
to exist *before* the game starts or it will not be found, so the runner
creates this first and holds it open for the whole run.

REQUIREMENTS
------------
/dev/uinput must be present and writable. In Docker: --device=/dev/uinput
(the container runs as root, so no extra group is needed). On a workstation
the invoking user must be in the `input` group or run as root.
"""

from __future__ import annotations

import time

try:
    from evdev import UInput, ecodes
    HAVE_EVDEV = True
except ImportError:                                   # pragma: no cover
    HAVE_EVDEV = False


# Soku's defaults: Z confirms, arrow keys move.
KEY_CONFIRM = "KEY_Z"
KEY_UP = "KEY_UP"
KEY_DOWN = "KEY_DOWN"
KEY_LEFT = "KEY_LEFT"
KEY_RIGHT = "KEY_RIGHT"

# Keys the virtual device advertises. dinput will not report a key the device
# never claimed to have, so declare everything the game might read.
_CAPS = [
    ecodes.KEY_Z, ecodes.KEY_X, ecodes.KEY_C, ecodes.KEY_V,
    ecodes.KEY_A, ecodes.KEY_S, ecodes.KEY_D,
    ecodes.KEY_UP, ecodes.KEY_DOWN, ecodes.KEY_LEFT, ecodes.KEY_RIGHT,
    ecodes.KEY_ENTER, ecodes.KEY_ESC, ecodes.KEY_SPACE,
] if HAVE_EVDEV else []


class VirtualKeyboardError(RuntimeError):
    pass


class VirtualKeyboard:
    """A uinput keyboard, scoped to a `with` block."""

    def __init__(self, name: str = "sfe-virtual-keyboard"):
        self.name = name
        self._ui = None

    def __enter__(self) -> "VirtualKeyboard":
        if not HAVE_EVDEV:
            raise VirtualKeyboardError(
                "python-evdev is not installed; cannot create a virtual keyboard"
            )
        try:
            self._ui = UInput({ecodes.EV_KEY: _CAPS}, name=self.name)
        except (PermissionError, FileNotFoundError, OSError) as e:
            raise VirtualKeyboardError(
                f"cannot open /dev/uinput ({e}). In Docker pass "
                f"--device=/dev/uinput; on a workstation join the `input` group."
            ) from e

        # Give udev/Wine a moment to notice the new device. Without this the
        # game can start enumerating before the device is fully registered.
        time.sleep(1.0)
        return self

    def __exit__(self, *exc) -> None:
        if self._ui is not None:
            self._ui.close()
            self._ui = None

    def tap(self, key_name: str, hold_s: float = 0.05) -> None:
        """Press and release one key.

        The hold is a few frames' worth: too short and the game's per-tick
        polling can miss it entirely, too long and key-repeat overshoots the
        menu.
        """
        if self._ui is None:
            raise VirtualKeyboardError("keyboard is not open")
        code = getattr(ecodes, key_name)
        self._ui.write(ecodes.EV_KEY, code, 1)   # down
        self._ui.syn()
        time.sleep(hold_s)
        self._ui.write(ecodes.EV_KEY, code, 0)   # up
        self._ui.syn()


def available() -> tuple[bool, str]:
    """Whether a virtual keyboard can be created here, and why not if it can't."""
    if not HAVE_EVDEV:
        return False, "python-evdev not installed"
    try:
        ui = UInput({ecodes.EV_KEY: _CAPS}, name="sfe-probe")
        ui.close()
        return True, "ok"
    except Exception as e:
        return False, str(e)
