"""Re-check a captured corpus against the digests recorded when it was made.

    python3 -m pipeline.verify out/            # every worker's manifest
    python3 -m pipeline.verify out/ --deep     # also fully decode each video

WHY THIS IS SEPARATE FROM validate.py
-------------------------------------
They answer different questions, at different times:

    validate.py   was this capture correct when it was made?
    verify.py     is it still what was made?

The first runs once, at capture time, and checks internal consistency: frame
counters, row/frame agreement, the input bitmask against its own boolean
columns. The second runs before you train, and checks nothing has changed on
disk since.

That distinction stops mattering only on hardware you trust. On a machine
with non-ECC memory, a consumer HDD and a corpus that will sit for weeks
before a training run, silent corruption after capture is a real failure mode
and nothing else in the pipeline would notice: an mp4 with a flipped bit
still decodes, and a CSV with a flipped digit still parses into a plausible
input.

WHAT EACH LAYER CATCHES
-----------------------
    corruption during capture, in the labels   validate.py, csv.mask_bits
        (the mask and its boolean columns are written from one value, so any
        disagreement between them is corruption -- a free parity check)
    corruption during capture, in the video    validate.py, csv/video counts
    truncation at any point                    validate.py, packet count
    corruption after capture, anywhere         this module

Nothing catches a bit flip that lands in the frame buffer before the encoder
reads it. That would need the game run twice and the outputs compared, which
`--sample` in a future revision could do cheaply for a subset -- Soku replays
are deterministic, so two captures of one replay must produce identical
inputs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def sha256(path: Path) -> str | None:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def decodes_cleanly(video: Path) -> tuple[bool, str]:
    """Fully decode the video, reporting the first error FFmpeg raises.

    Expensive -- this is the check that costs ~14 s per capture, which is why
    it is opt-in here and not run at capture time. It is worth paying on
    unreliable hardware, because H.264 carries no checksums: a corrupted
    macroblock decodes into visible garbage without any error at the
    container level.
    """
    try:
        res = subprocess.run(
            ["ffmpeg", "-v", "error", "-i", str(video), "-f", "null", "-"],
            capture_output=True, text=True, timeout=900,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        return False, str(e)
    err = res.stderr.strip()
    return (res.returncode == 0 and not err), err.splitlines()[0] if err else ""


def manifests(root: Path) -> list[Path]:
    found = sorted(root.rglob("manifest.jsonl"))
    return found or ([root / "manifest.jsonl"]
                     if (root / "manifest.jsonl").exists() else [])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("root", type=Path, help="dataset root (may contain w*/ shards)")
    ap.add_argument("--deep", action="store_true",
                    help="also fully decode every video (slow; ~14 s each)")
    ap.add_argument("--quiet", action="store_true", help="only report problems")
    args = ap.parse_args(argv)

    mfs = manifests(args.root)
    if not mfs:
        print(f"error: no manifest.jsonl under {args.root}", file=sys.stderr)
        return 2

    checked = ok = 0
    missing_digest = 0
    problems: list[str] = []

    for mf in mfs:
        for line in mf.read_text(errors="ignore").splitlines():
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            if e.get("status") != "ok":
                continue
            checked += 1
            rid = e.get("replay_id", "?")

            for kind, key in (("video", "video_sha256"), ("csv", "csv_sha256")):
                path = e.get(kind)
                want = e.get(key)
                if not path:
                    continue
                if not want:
                    # Captured before digests were recorded. Not a failure,
                    # but say so rather than silently reporting success -- an
                    # unverifiable file must not look like a verified one.
                    missing_digest += 1
                    continue
                p = Path(path)
                if not p.exists():
                    problems.append(f"{rid}: {kind} missing from disk ({p})")
                    continue
                got = sha256(p)
                if got != want:
                    problems.append(
                        f"{rid}: {kind} DIGEST MISMATCH — "
                        f"recorded {want[:16]}…, now {(got or 'unreadable')[:16]}…"
                    )

            if args.deep and e.get("video"):
                good, why = decodes_cleanly(Path(e["video"]))
                if not good:
                    problems.append(f"{rid}: video does not decode cleanly — {why}")

            if not problems or not problems[-1].startswith(rid):
                ok += 1

    print(f"checked : {checked} capture(s) across {len(mfs)} manifest(s)")
    print(f"verified: {ok}")
    if missing_digest:
        print(f"UNVERIFIABLE: {missing_digest} artefact(s) predate digest "
              f"recording — re-capture or accept on trust")
    if problems:
        print(f"\nPROBLEMS ({len(problems)}):")
        for p in problems:
            print(f"  {p}")
        return 1
    if not args.quiet:
        print("\nno corruption detected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
