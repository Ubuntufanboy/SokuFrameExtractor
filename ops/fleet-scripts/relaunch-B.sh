#!/bin/bash
# The swarm computes its shard list once, at start. B was launched while the
# corpus was still being scraped, so its workers only ever saw the first 108
# replays. Wait for the scrape to finish, then relaunch across the full corpus.
# --resume keeps everything already captured.
until ! pgrep -f fetch_replays > /dev/null; do sleep 30; done
cd /root/sfe
python3 -m runner.swarm stop --out /root/dataset --workers 10 >/dev/null 2>&1
sleep 8
pkill -x th123.exe 2>/dev/null
sleep 3
# Budget from real free space, keeping a 6 GB floor.
FREE_GB=$(df -BG --output=avail / | tail -1 | tr -dc 0-9)
USED_GB=$(du -sBG /root/dataset 2>/dev/null | cut -f1 | tr -dc 0-9)
BUDGET=$(( FREE_GB + USED_GB - 6 ))
echo "relaunching over $(ls /root/corpus/*.rep | wc -l) replays, budget ${BUDGET} GB"
python3 -m runner.swarm start --workers 10 --cpus 4 \
  --prefix /root/wineprefix --prefixes /root/prefixes \
  --corpus /root/corpus --out /root/dataset \
  --budget-gb "$BUDGET" --timeout 1800 2>&1 | tail -4
