#!/bin/bash
# Full-corpus state-label capture: fetch the sources, then two CSV-only workers.
#
# Two workers, not more: each needs its own Wine prefix and its own game
# directory (one wineserver per prefix), and the box has twelve cores split
# into two blocks of six.
#
# --gzip-csv is not optional at this width. The sidecar carries 24 projectile
# slots per player on top of the 26 state fields, which is about 5.5 MB a
# replay uncompressed and 0.9 MB gzipped -- 11 GB against 1.9 GB over the
# corpus. It compresses this well because most slots are zero most of the time.
#
# --resume makes this restartable: it skips whatever the manifest already
# records as ok, so an interrupted run picks up where it stopped rather than
# recapturing 20 hours of replays.
set -u
cd "$HOME/sfe"
echo "=== fetching sources $(date -Is) ==="
python3 -m pipeline.fetch_corpus_reps "$HOME/corpus/manifest.jsonl" \
        -o "$HOME/corpus-reps"
echo "=== capturing $(date -Is) ==="
start=$(date +%s)
for i in 0 1; do
  g=$HOME/sfe-game; [ $i -gt 0 ] && g=$HOME/sfe-game$i
  SFE_GAME=$g SFE_PREFIX=$HOME/st-prefix$i SFE_OUT=$HOME/state-out$i \
  SFE_REPLAYS=$HOME/corpus-reps SFE_CPU_BLOCK=$i \
  "$HOME/sfe/ops/bwrap/sfe-bwrap" /app/docker/entrypoint.sh \
      --replay-dir /replays --out /out --shard $i/2 --no-video --resume \
      --gzip-csv --cpus 6 --min-free-gb 0.6 --timeout 900 \
      > "$HOME/st$i.log" 2>&1 &
done
wait
echo "=== done $(date -Is), elapsed $(( $(date +%s) - start ))s ==="
