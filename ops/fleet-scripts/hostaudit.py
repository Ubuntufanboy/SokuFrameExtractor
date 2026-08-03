"""Per-host integrity audit: ids, validation stats, storage."""
import json, glob, os, sys, subprocess, collections
root = sys.argv[1]
ids, dup = set(), []
ok = fail = frames = 0
vid = 0
zero_input = 0
sync_bad = []
mask_bad = []
for mf in sorted(glob.glob(os.path.join(root, "w*", "manifest.jsonl"))):
    for line in open(mf, errors="ignore"):
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        rid = e.get("replay_id")
        if e.get("status") != "ok":
            fail += 1
            continue
        ok += 1
        if rid in ids:
            dup.append(rid)
        ids.add(rid)
        frames += e.get("frames") or 0
        v = e.get("validation") or {}
        if v.get("csv_rows") != v.get("video_frames"):
            sync_bad.append((rid, v.get("csv_rows"), v.get("video_frames")))
        if v.get("mask_mismatches"):
            mask_bad.append((rid, v["mask_mismatches"]))
        if v.get("duplicate_rows"):
            pass
        try:
            vid += os.path.getsize(e["video"])
        except (KeyError, OSError):
            pass

# orphan capture dirs (video on disk, not in any manifest)
listed = set()
for mf in glob.glob(os.path.join(root, "w*", "manifest.jsonl")):
    for line in open(mf, errors="ignore"):
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        if e.get("video"):
            listed.add(os.path.realpath(e["video"]))
orph = [v for v in glob.glob(os.path.join(root, "w*", "*", "video.mp4"))
        if os.path.realpath(v) not in listed]

free = subprocess.run(["bash","-c","df -BG --output=avail / | tail -1 | tr -dc 0-9"],
                      capture_output=True, text=True).stdout.strip()
print(json.dumps({
    "ok": ok, "fail": fail, "hours": round(frames/60/3600, 2),
    "gb": round(vid/1e9, 2), "unique_ids": len(ids),
    "dup_within_host": dup[:5], "n_dup": len(dup),
    "sync_mismatch": sync_bad[:5], "n_sync": len(sync_bad),
    "mask_mismatch": mask_bad[:5], "n_mask": len(mask_bad),
    "orphans": len(orph), "free_gb": int(free or 0),
    "ids": sorted(ids),
}))
