"""The manifest: one JSON line per attempted replay.

Append-only JSONL rather than a database, for two reasons. It survives a kill
-9 mid-run with at most one truncated line, and `--resume` is a set difference
over a file you can read with `less`.

Every entry records the source .rep's content hash. That is what makes a
capture attributable: the pre-refactor output named directories from a counter
(replay_001, replay_002...) and never recorded which file produced them, so a
single navigation slip silently relabelled everything downstream with no way to
detect it afterwards.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

MANIFEST_NAME = "manifest.jsonl"


def replay_id(rep_path: Path) -> str:
    """Stable directory name for a replay: <stem>-<sha8 of contents>.

    The hash disambiguates the many identically-named replays Soku produces
    and doubles as an integrity check on the source file.
    """
    digest = hashlib.sha256(rep_path.read_bytes()).hexdigest()
    stem = "".join(c if c.isalnum() or c in "-_" else "_" for c in rep_path.stem)
    return f"{stem[:60]}-{digest[:8]}"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


@dataclass
class Entry:
    replay_id: str
    source_rep: str          # absolute path as staged from
    source_sha256: str
    status: str              # ok | failed | timeout | invalid
    frames: int = 0
    elapsed_ms: int = 0
    reason: str | None = None
    video: str | None = None
    csv: str | None = None
    # Digests of what was written, taken once at capture time.
    #
    # validate.py answers "was this capture correct when it was made?".
    # These answer the different question "is it still what was made?" --
    # which is the one that matters on a machine with non-ECC memory, a
    # consumer HDD, and a training run that will read the corpus weeks later.
    # Silent bit rot between capture and training is otherwise undetectable:
    # an mp4 with a flipped bit still decodes, and a CSV with a flipped digit
    # still parses.
    video_sha256: str | None = None
    csv_sha256: str | None = None
    validation: dict = field(default_factory=dict)
    meta: dict = field(default_factory=dict)

    def to_json(self) -> str:
        return json.dumps(asdict(self), separators=(",", ":"))


def append(root: Path, entry: Entry) -> None:
    root.mkdir(parents=True, exist_ok=True)
    with open(root / MANIFEST_NAME, "a") as fh:
        fh.write(entry.to_json() + "\n")
        fh.flush()


def load(root: Path) -> list[dict]:
    """Read the manifest, skipping any trailing partial line from a hard kill."""
    path = root / MANIFEST_NAME
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def completed_ids(root: Path) -> set[str]:
    """Replay ids that succeeded. Used by --resume.

    Only "ok" counts: a failed or timed-out replay should be retried on the
    next run, since the cause is usually transient (a wedged wineserver, a
    missed menu transition) rather than anything about the file itself.
    """
    return {e["replay_id"] for e in load(root) if e.get("status") == "ok"}


def attempted_ids(root: Path) -> set[str]:
    return {e["replay_id"] for e in load(root)}
