"""Remove replays from a corpus that a manifest says are already captured.

    python3 -m runner.prune_corpus /root/corpus /root/dataset

Used when moving collection between machines. The alternative -- letting the
new host recapture what the old one already did -- wastes exactly as much
compute as the old host managed, and the simpler alternatives are worse:

  * Re-sharding onto the new worker count changes which worker owns which
    replay, so the per-worker `--resume` manifests no longer line up.
  * Seeding every new worker's manifest with the old entries makes each
    worker's manifest claim captures it did not make, and makes any later
    audit re-check the same files once per worker.

Deleting the inputs is the honest version of "already done": the work is
recorded once, in the manifest that actually produced it, and the corpus
contains only what remains.

Captures that failed are deliberately NOT pruned -- a replay the old host
could not capture is exactly the kind the new one should retry.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def completed_stems(dataset: Path) -> set[str]:
    """Replay stems recorded as successfully captured, from every manifest."""
    stems: set[str] = set()
    for mf in dataset.rglob("manifest.jsonl"):
        for line in mf.read_text(errors="ignore").splitlines():
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            if e.get("status") != "ok":
                continue
            rid = e.get("replay_id") or ""
            # replay_id is "<stem>-<sha8>"; the corpus names files "<stem>.rep".
            stem = rid.rsplit("-", 1)[0] if "-" in rid else rid
            if stem:
                stems.add(stem)
    return stems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("corpus", type=Path)
    ap.add_argument("dataset", type=Path)
    ap.add_argument("--apply", action="store_true",
                    help="actually delete; without this it only reports")
    args = ap.parse_args(argv)

    if not args.corpus.is_dir():
        print(f"error: no corpus at {args.corpus}", file=sys.stderr)
        return 2

    done = completed_stems(args.dataset)
    reps = sorted(args.corpus.glob("*.rep"))
    hit = [p for p in reps if p.stem in done]

    print(f"corpus     : {len(reps)} replays")
    print(f"already ok : {len(done)} in {args.dataset}")
    print(f"to remove  : {len(hit)}")
    print(f"remaining  : {len(reps) - len(hit)}")

    if not args.apply:
        print("\n(dry run — pass --apply to delete)")
        return 0

    for p in hit:
        p.unlink()
    print(f"\nremoved {len(hit)}; corpus now {len(list(args.corpus.glob('*.rep')))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
