"""The .ini, the C++ loader, and the Config struct must describe the same keys.

These three drifted apart badly in the pre-refactor tree, in both directions at
once:

  * ``SokuFrameExtractor.ini`` advertised ``SaveAsBMP``, ``EncoderThreads`` and
    ``UseRenderTarget``. No code had ever read any of them.
  * ``loadConfig()`` read ``FifoPath`` and ``UseVAAPI``, neither of which
    appeared in the shipped .ini.
  * ``SkipFrames`` and ``UseVAAPI`` were parsed into ``Config``, logged at
    startup so they looked live, and then read by nothing.

Every one of those is invisible at runtime: you set a documented option, the
log dutifully echoes it back, and nothing happens. This test makes that
condition a build failure instead.

Run with: python3 -m pytest tests/ -q   (or plain: python3 tests/test_config_parity.py)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INI = ROOT / "config" / "sfe.ini"
MAIN_CPP = ROOT / "dll" / "src" / "main.cpp"
CONFIG_HPP = ROOT / "dll" / "include" / "sfe" / "config.hpp"


def ini_keys() -> set[str]:
    """Keys defined under [General] in the shipped .ini."""
    keys: set[str] = set()
    in_general = False
    for line in INI.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("["):
            in_general = line.lower() == "[general]"
            continue
        if in_general and "=" in line:
            keys.add(line.split("=", 1)[0].strip())
    return keys


def loader_keys() -> set[str]:
    """Keys the C++ loader actually asks for.

    Matches the readStr("Key", ...) / readBool("Key", ...) call sites in
    loadConfig(). Deliberately literal: if someone builds a key name
    dynamically this will under-report, and that is a thing worth noticing.
    """
    src = MAIN_CPP.read_text()
    body = _extract_function(src, "loadConfig")
    return set(re.findall(r'read(?:Str|Bool)\s*\(\s*"([^"]+)"', body))


def config_fields() -> set[str]:
    """Field names of struct Config."""
    src = CONFIG_HPP.read_text()
    m = re.search(r"struct Config\s*\{(.*?)\n\};", src, re.S)
    assert m, "could not locate struct Config in config.hpp"
    body = m.group(1)
    body = re.sub(r"//[^\n]*", "", body)  # strip comments
    # Matches both plain scalars and the fixed char buffers that replaced
    # std::string when the MSVC C++ runtime dependency was removed from the
    # DLL (see dll/include/sfe/config.hpp).
    return set(
        re.findall(r"^\s*(?:std::string|bool|int|char)\s+(\w+)\s*(?:\[|=|;)", body, re.M)
    )


def _extract_function(src: str, name: str) -> str:
    start = src.index(f"{name}(")
    depth, i = 0, src.index("{", start)
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i : j + 1]
    raise AssertionError(f"unbalanced braces in {name}")


def _to_field(key: str) -> str:
    """ReplayDir -> replay_dir"""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", key).lower()


def test_ini_and_loader_agree() -> None:
    ini, loader = ini_keys(), loader_keys()
    assert ini == loader, (
        f"config/sfe.ini and loadConfig() disagree.\n"
        f"  in .ini but never read : {sorted(ini - loader)}\n"
        f"  read but not in .ini   : {sorted(loader - ini)}"
    )


def test_every_config_field_is_loaded() -> None:
    """No field may exist that nothing populates -- that is how dead options happen."""
    expected = {_to_field(k) for k in loader_keys()}
    actual = config_fields()
    assert expected == actual, (
        f"struct Config and loadConfig() disagree.\n"
        f"  field with no .ini key : {sorted(actual - expected)}\n"
        f"  key with no field      : {sorted(expected - actual)}"
    )


def test_no_known_dead_options_return() -> None:
    """Guard the specific names that were dead before, so they cannot creep back."""
    dead = {"SaveAsBMP", "EncoderThreads", "UseRenderTarget", "SkipFrames", "UseVAAPI"}
    present = dead & (ini_keys() | loader_keys())
    assert not present, f"previously-dead options reintroduced: {sorted(present)}"


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"PASS  {name}")
            except AssertionError as e:
                failures += 1
                print(f"FAIL  {name}\n      {e}")
    sys.exit(1 if failures else 0)
