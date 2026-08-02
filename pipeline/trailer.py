"""Stitch a showcase reel from several captures, with input overlays burned in.

Produces the clip that goes in the README. Clips are chosen by input density
rather than taken from the start of each replay: a fighting-game match is
mostly neutral spacing, and the opening seconds are usually two characters
walking at each other. Picking the busiest window lands on actual exchanges.

    python3 -m pipeline.trailer out/ -o docs/assets/showcase.mp4

Reads the manifest so only captures that passed validation are used -- a reel
built from corrupt captures would advertise the bug rather than the tool.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from . import manifest, overlay

FPS = 60


def pick_captures(root: Path, limit: int) -> list[Path]:
    """Directories of successful captures, newest first."""
    entries = [e for e in manifest.load(root) if e.get("status") == "ok"]
    dirs: list[Path] = []
    for e in reversed(entries):
        d = root / e["replay_id"]
        if (d / "video.mp4").exists() and (d / "inputs.csv").exists():
            dirs.append(d)
        if len(dirs) >= limit:
            break

    if not dirs:
        # Fall back to scanning, so the tool still works before a manifest
        # exists (e.g. captures produced by hand).
        for d in sorted(root.iterdir()):
            if d.is_dir() and (d / "video.mp4").exists() and (d / "inputs.csv").exists():
                dirs.append(d)
            if len(dirs) >= limit:
                break
    return dirs


def build(
    root: Path,
    out: Path,
    *,
    clips: int = 4,
    seconds: float = 6.0,
    n_cpus: int = 0,
) -> None:
    captures = pick_captures(root, clips)
    if not captures:
        raise SystemExit(f"no usable captures under {root}")

    count = int(seconds * FPS)
    out.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        parts: list[Path] = []
        for i, cap in enumerate(captures):
            csv_path = cap / "inputs.csv"
            start = overlay.busiest_window(csv_path, count)
            part = tmp / f"part{i:02d}.mp4"
            print(f"  clip {i+1}/{len(captures)}: {cap.name} @ frame {start}")
            overlay.render(
                cap / "video.mp4", csv_path, part,
                start=start, count=count, n_cpus=n_cpus,
            )
            parts.append(part)

        # Concat demuxer rather than the filter: every part was produced by the
        # same encoder settings, so the streams are already compatible and this
        # avoids a second full re-encode.
        listing = tmp / "parts.txt"
        listing.write_text("".join(f"file '{p}'\n" for p in parts))

        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "concat", "-safe", "0", "-i", str(listing),
            "-c", "copy", str(out),
        ]
        from runner import throttle
        subprocess.run(throttle.wrap(cmd, n_cpus=n_cpus), check=True)

    print(f"wrote {out} ({len(captures)} clips, {seconds:g}s each)")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Build a showcase reel from captures.")
    ap.add_argument("root", type=Path, help="capture root (contains manifest.jsonl)")
    ap.add_argument("-o", "--out", type=Path,
                    default=Path("docs/assets/showcase.mp4"))
    ap.add_argument("--clips", type=int, default=4)
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--cpus", type=int, default=0)
    args = ap.parse_args(argv)

    if not args.root.exists():
        print(f"no such path: {args.root}", file=sys.stderr)
        return 2
    build(args.root, args.out, clips=args.clips, seconds=args.seconds,
          n_cpus=args.cpus)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
