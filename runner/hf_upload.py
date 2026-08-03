"""Stage completed captures to a Hugging Face dataset repo, incrementally.

    python3 -m runner.hf_upload --roots /root/dataset2 /root/dataset \
        --repo Smashlytics/soku-frames --host A --shard-gb 2

Why this exists: moving the corpus host-to-host over SSH measured ~4.7 MB/s
aggregate, because the collection hosts are CPU-saturated and ssh has to encrypt
every byte. A CDN-backed object store is the faster intermediary, and it also
decouples the upload (which can run while collection finishes) from the download
(which happens when the training box exists).

Three constraints shape the design:

* **Disk.** Host A runs with ~15 GB free while still writing captures. Shards are
  built one at a time and deleted the moment the upload is confirmed, so the
  extra footprint is one shard, not a second copy of the corpus.
* **CPU belongs to collection.** Runs at nice 19, and tars without compression --
  h264 does not compress, so `-z` would burn a core to make the file bigger.
* **Resumability.** Every capture that lands in a shard is recorded locally
  *after* the upload returns. An interrupted run re-uploads at most one shard,
  never a finished one.

Shards are tar archives of whole capture directories, ~2 GB each. Uploading the
~4,000 capture directories as individual files instead would mean three HTTP
round trips per capture and a far slower download.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import List

STATE = "hf_uploaded.txt"


def ok_captures(roots: List[Path]) -> List[tuple[Path, str, int]]:
    """(dataset_root, relative capture dir, frames) for every ok capture."""
    out, seen = [], set()
    for root in roots:
        for mf in sorted(root.rglob("manifest.jsonl")):
            for line in mf.read_text(errors="ignore").splitlines():
                try:
                    e = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if e.get("status") != "ok":
                    continue
                rid = e.get("replay_id")
                video = e.get("video") or ""
                if not rid or not video or rid in seen:
                    continue
                d = Path(video)
                d = d if d.is_absolute() else (mf.parent / d)
                d = d.parent
                if not (d / "video.mp4").exists() or not (d / "inputs.csv").exists():
                    continue
                seen.add(rid)
                out.append((root, str(d.relative_to(root)), int(e.get("frames") or 0)))
    return out


def dir_bytes(p: Path) -> int:
    return sum(f.stat().st_size for f in p.rglob("*") if f.is_file())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--roots", nargs="+", required=True, type=Path)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--host", required=True, help="label; prefixes shard names")
    ap.add_argument("--shard-gb", type=float, default=2.0)
    ap.add_argument("--work", type=Path, default=Path("/root/hf_stage"))
    ap.add_argument("--min-free-gb", type=float, default=6.0,
                    help="refuse to build a shard below this much free space")
    ap.add_argument("--max-shards", type=int, default=0, help="0 = no limit")
    ap.add_argument("--loop", action="store_true",
                    help="keep polling for newly completed captures")
    ap.add_argument("--poll-seconds", type=int, default=300)
    args = ap.parse_args()

    from huggingface_hub import HfApi

    api = HfApi()
    args.work.mkdir(parents=True, exist_ok=True)
    state = args.work / STATE
    done = set(state.read_text().split()) if state.exists() else set()
    print(f"{len(done)} captures already uploaded", flush=True)

    shard_bytes = int(args.shard_gb * (1 << 30))

    # Shard numbering comes from what is already in the repo, not from local
    # files. Deriving it locally is wrong in a way that destroys data: shards are
    # deleted after upload, so a fresh run finds no local tars, restarts at
    # 0000, and silently overwrites the shards already on the Hub. A failed
    # upload leaving its tar behind makes the numbering wrong in the other
    # direction too. The repo is the only authority on what exists.
    try:
        existing = [f for f in api.list_repo_files(args.repo, repo_type="dataset")
                    if f.startswith(f"shards/{args.host}-")]
    except Exception as exc:
        print(f"cannot list {args.repo}: {exc}", file=sys.stderr)
        return 4
    used = set()
    for f in existing:
        stem = Path(f).stem                      # "C-0007"
        try:
            used.add(int(stem.rsplit("-", 1)[1]))
        except (IndexError, ValueError):
            continue
    next_idx = max(used) + 1 if used else 0
    n_shards = 0
    print(f"{len(existing)} shard(s) already in {args.repo} for host "
          f"{args.host}; next index {next_idx:04d}", flush=True)

    while True:
        caps = [c for c in ok_captures(args.roots) if f"{c[0]}::{c[1]}" not in done]
        if not caps:
            if not args.loop:
                print("nothing left to upload")
                return 0
            time.sleep(args.poll_seconds)
            continue

        # Build one shard: accumulate capture dirs until the size target is hit.
        batch, total = [], 0
        for root, rel, frames in caps:
            sz = dir_bytes(root / rel)
            if total and total + sz > shard_bytes:
                break
            batch.append((root, rel, frames, sz))
            total += sz

        st = os.statvfs(args.work)
        free_gb = st.f_bavail * st.f_frsize / (1 << 30)
        if free_gb - total / (1 << 30) < args.min_free_gb:
            print(f"free space {free_gb:.1f} GB would drop below "
                  f"{args.min_free_gb} GB building this shard; waiting", flush=True)
            if not args.loop:
                return 2
            time.sleep(args.poll_seconds)
            continue

        name = f"{args.host}-{next_idx:04d}.tar"
        tar_path = args.work / name
        tar_path.unlink(missing_ok=True)     # drop any tar left by a failed run

        # One tar per dataset root, since paths are relative to their own root.
        by_root: dict[Path, list[str]] = {}
        for root, rel, _, _ in batch:
            by_root.setdefault(root, []).append(rel)

        t0 = time.time()
        first = True
        for root, rels in by_root.items():
            flag = "-cf" if first else "-rf"
            proc = subprocess.run(
                ["nice", "-n", "19", "tar", flag, str(tar_path), "-C", str(root)] + rels,
                capture_output=True, text=True,
            )
            if proc.returncode != 0:
                print(f"tar failed: {proc.stderr[:300]}", file=sys.stderr)
                tar_path.unlink(missing_ok=True)
                return 3
            first = False

        size = tar_path.stat().st_size
        tar_s = time.time() - t0

        index = [{"root": str(r), "rel": rel, "frames": f} for r, rel, f, _ in batch]
        (args.work / f"{name}.json").write_text(json.dumps(index))

        t1 = time.time()
        api.upload_file(path_or_fileobj=str(tar_path), path_in_repo=f"shards/{name}",
                        repo_id=args.repo, repo_type="dataset")
        api.upload_file(path_or_fileobj=str(args.work / f"{name}.json"),
                        path_in_repo=f"index/{name}.json",
                        repo_id=args.repo, repo_type="dataset")
        up_s = time.time() - t1

        # Record only after the upload returns, so a crash re-does one shard.
        with state.open("a") as fh:
            for root, rel, _, _ in batch:
                fh.write(f"{root}::{rel}\n")
        done.update(f"{r}::{rel}" for r, rel, _, _ in batch)

        tar_path.unlink(missing_ok=True)
        (args.work / f"{name}.json").unlink(missing_ok=True)
        next_idx += 1
        n_shards += 1

        hours = sum(f for _, _, f, _ in batch) / 60 / 3600
        print(f"{name}: {len(batch)} captures, {hours:.2f} h, {size/1e9:.2f} GB | "
              f"tar {tar_s:.0f}s, upload {up_s:.0f}s = {size/1e6/max(up_s,1e-9):.1f} MB/s "
              f"({size*8/1e6/max(up_s,1e-9):.0f} Mb/s)", flush=True)

        if args.max_shards and n_shards >= args.max_shards:
            return 0


if __name__ == "__main__":
    raise SystemExit(main())
