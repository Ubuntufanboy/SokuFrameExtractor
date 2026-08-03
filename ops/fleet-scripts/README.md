# Fleet operational scripts

Ad-hoc scripts that lived on the collection hosts rather than in the repo,
captured here before those hosts were deleted.

* `hostaudit.py` — per-host audit: ok/fail counts, csv/video row agreement,
  mask-parity failures, orphan capture directories. Used for the pre-training
  integrity audit.
* `inputcheck.py` — spot-checks an `inputs.csv` against its video.
* `relaunch-B.sh`, `launch-C.sh`, `onstart.sh` — swarm launch wrappers as they
  were actually invoked on each host.

The captures themselves, the per-capture manifests (with capture-time SHA-256
digests and validation records), and the source `.rep` corpora are on the
Hugging Face staging repos under `metadata/` — see `runner/hf_upload.py`.
