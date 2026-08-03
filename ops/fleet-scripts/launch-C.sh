#!/bin/bash
# Wait for the scrape before launching: runner/swarm.py computes its shard list
# once at start, so a swarm launched mid-scrape only ever sees the replays that
# existed at that moment.
until ! pgrep -f fetch_replays > /dev/null; do sleep 30; done
cd /root/sfe
FREE_GB=$(df -BG --output=avail / | tail -1 | tr -dc 0-9)
BUDGET=$(( FREE_GB - 8 ))
echo "launching over $(ls /root/corpus/*.rep | wc -l) replays, budget ${BUDGET} GB"
python3 -m runner.swarm start --workers 7 --cpus 4 \
  --prefix /root/wineprefix --prefixes /root/prefixes \
  --corpus /root/corpus --out /root/dataset \
  --budget-gb "$BUDGET" --timeout 1800 2>&1 | grep -vE "MB copied" | tail -5
