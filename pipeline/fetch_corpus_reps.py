"""Re-fetch the .rep sources for a corpus that already exists.

    python3 -m pipeline.fetch_corpus_reps ~/corpus/manifest.jsonl -o ~/corpus-reps

WHY THIS IS NEEDED AT ALL
-------------------------
A capture directory keeps the video and the sidecar, not the replay that
produced them. So adding a column to the sidecar -- which is what the game
state work needs -- means running the game again, which means having the .rep
again. The corpus on this machine was built from a page of the archive whose
tarball is no longer around; only the ids survive, in the manifest.

VERIFICATION IS FREE, SO IT IS NOT OPTIONAL
-------------------------------------------
`manifest.replay_id` is `<stem>-<first 8 of sha256(contents)>`, so every
directory name in the corpus already carries a digest of the exact bytes that
produced it. Re-downloading by id and recomputing that digest therefore proves
the new file is the same replay, not merely one with the same number -- which
matters because a sidecar captured from a *different* replay would align badly
against the corpus video and there would be nothing in the data to say why.

Anything that does not match is written to the report and left out, rather
than captured and quietly paired with the wrong video.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import urllib.error
from pathlib import Path

from .fetch_replays import DEFAULT_DELAY_S, Fetcher, download_one


def wanted(manifest: Path) -> list[tuple[int, str, str]]:
    """[(numeric id, expected sha8, full replay_id)] from a capture manifest."""
    out, seen = [], set()
    for line in manifest.read_text().splitlines():
        if not line.strip():
            continue
        rid = json.loads(line).get("replay_id")
        if not rid or rid in seen:
            continue
        seen.add(rid)
        stem, _, sha8 = rid.rpartition("-")
        if not stem.isdigit():
            # Locally recorded replays have names, not archive ids, and cannot
            # be re-fetched. Say so rather than failing the whole run.
            print(f"skip {rid}: not an archive id", file=sys.stderr)
            continue
        out.append((int(stem), sha8, rid))
    return out


def sha8(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()[:8]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("manifest", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--delay", type=float, default=DEFAULT_DELAY_S)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args(argv)

    args.out.mkdir(parents=True, exist_ok=True)
    todo = wanted(args.manifest)
    if args.limit:
        todo = todo[: args.limit]

    f = Fetcher(delay_s=args.delay)
    ok = skipped = mismatched = failed = 0
    report = args.out / "fetch_report.jsonl"

    with report.open("a") as rep:
        for i, (num, want, rid) in enumerate(todo, 1):
            dest = args.out / f"{num}.rep"
            if dest.exists() and sha8(dest) == want:
                skipped += 1
                continue
            try:
                download_one(f, num, dest)
            except (urllib.error.HTTPError, urllib.error.URLError,
                    TimeoutError, OSError) as e:
                failed += 1
                rep.write(json.dumps({"id": num, "replay_id": rid,
                                      "error": str(e)}) + "\n")
                continue
            got = sha8(dest)
            if got != want:
                # Same id, different bytes. Do not capture it: the sidecar
                # would be paired with a video of a different match.
                mismatched += 1
                dest.unlink(missing_ok=True)
                rep.write(json.dumps({"id": num, "replay_id": rid,
                                      "expected": want, "got": got}) + "\n")
                continue
            ok += 1
            if i % 100 == 0:
                print(f"[{i}/{len(todo)}] ok={ok} skip={skipped} "
                      f"mismatch={mismatched} fail={failed}", flush=True)

    print(f"\nfetched {ok}, already had {skipped}, "
          f"hash mismatch {mismatched}, failed {failed}, of {len(todo)}")
    print(f"report: {report}")
    return 0 if (ok + skipped) else 1


if __name__ == "__main__":
    raise SystemExit(main())
