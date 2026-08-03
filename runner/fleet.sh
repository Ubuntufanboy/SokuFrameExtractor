#!/usr/bin/env bash
# Roll up collection status across every host.
#
#   runner/fleet.sh            status of all hosts
#   runner/fleet.sh stop       stop collection everywhere
#
# Hosts are "<label> <port> <host> <dataset-dir> <workers>". Collection is
# deliberately uncoordinated -- each host owns a disjoint slice of the replay
# listing (see pipeline/fetch_replays.py --start-offset) and writes its own
# manifests -- so this only reads. There is no shared state to corrupt and no
# scheduler to go wrong; the cost is that "how are we doing" needs asking each
# host, which is what this does.
set -uo pipefail

KEY="${SFE_KEY:-$HOME/.ssh/id_ed25519_ai}"
SSH_OPTS="-i $KEY -o BatchMode=yes -o ConnectTimeout=25 -o StrictHostKeyChecking=no"

# label port host dataset workers live
#
# `live=0` marks a directory that holds finished captures but is not being
# written to -- A0 is the archive migrated off the original 5.76-core box. Its
# footage counts toward the total; its *rate* must not, because dividing
# already-finished footage by the current swarm's uptime invents throughput
# that is not happening. That mistake put the fleet rate at 36.8 h/wall-h when
# the true figure was 13.7.
HOSTS=(
  "A  42373 184.144.255.144 /root/dataset2 16 1"
  "A0 42373 184.144.255.144 /root/dataset  16 0"
  "B  18577 184.144.255.144 /root/dataset  10 1"
  "C  22653 184.144.255.144 /root/dataset   7 1"
  "D  24358 45.135.163.226  /root/dataset   4 1"
)

CMD="${1:-status}"

read -r -d '' ROLLUP <<'PY' || true
import json, glob, os, sys, subprocess
root = sys.argv[1]
ok = fail = frames = 0
vid = 0
for mf in glob.glob(os.path.join(root, "w*", "manifest.jsonl")):
    for line in open(mf, errors="ignore"):
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        if e.get("status") == "ok":
            ok += 1
            frames += e.get("frames") or 0
            try:
                vid += os.path.getsize(e["video"])
            except (KeyError, OSError):
                pass
        else:
            fail += 1
try:
    el = int(subprocess.run(
        ["bash", "-c", "ps -o etimes= -p $(pgrep -f runner.collect | head -1) | tr -d ' '"],
        capture_output=True, text=True).stdout.strip() or 0)
except Exception:
    el = 0
alive = subprocess.run(["bash", "-c", "pgrep -fc runner.collect || true"],
                       capture_output=True, text=True).stdout.strip() or "0"
free = subprocess.run(["bash", "-c", "df -BG --output=avail / | tail -1 | tr -dc 0-9"],
                      capture_output=True, text=True).stdout.strip() or "0"
h = frames / 60 / 3600
rate = h / (el / 3600) if el else 0.0
mbph = vid / 1e6 / h if h else 0.0
print(f"{ok}|{fail}|{h:.2f}|{vid/1e9:.1f}|{rate:.2f}|{mbph:.0f}|{alive}|{free}")
PY

if [ "$CMD" = "stop" ]; then
    for spec in "${HOSTS[@]}"; do
        set -- $spec; label=$1; port=$2; host=$3; ds=$4; w=$5; live=${6:-1}
        [ "$live" = "1" ] || continue
        echo -n "  $label: "
        ssh $SSH_OPTS -p "$port" "root@$host" \
            "cd /root/sfe 2>/dev/null && python3 -m runner.swarm stop --out $ds --workers $w 2>&1 | tail -1" \
            2>/dev/null | tail -1 || echo "unreachable"
    done
    exit 0
fi

printf "%-4s %6s %5s %9s %7s %9s %8s %7s %6s\n" \
    host ok fail hours GB "h/wall-h" "MB/h" alive freeGB
tot_ok=0; tot_h=0; tot_gb=0; tot_rate=0
for spec in "${HOSTS[@]}"; do
    set -- $spec; label=$1; port=$2; host=$3; ds=$4; w=$5; live=${6:-1}
    out=$(ssh $SSH_OPTS -p "$port" "root@$host" "python3 - $ds" <<<"$ROLLUP" 2>/dev/null | tail -1)
    if [ -z "$out" ] || [[ "$out" != *"|"* ]]; then
        printf "%-4s %s\n" "$label" "unreachable"
        continue
    fi
    IFS='|' read -r ok fail hours gb rate mbph alive free <<<"$out"
    [ "$live" = "1" ] || rate="archive"
    printf "%-4s %6s %5s %9s %7s %9s %8s %7s %6s\n" \
        "$label" "$ok" "$fail" "$hours" "$gb" "$rate" "$mbph" "$alive" "$free"
    tot_ok=$((tot_ok + ok))
    tot_h=$(python3 -c "print(f'{$tot_h + $hours:.2f}')")
    tot_gb=$(python3 -c "print(f'{$tot_gb + $gb:.1f}')")
    [ "$live" = "1" ] && tot_rate=$(python3 -c "print(f'{$tot_rate + $rate:.2f}')")
done
echo
echo "  TOTAL: $tot_ok captures, ${tot_h} hours of footage, ${tot_gb} GB"
echo "  RATE : ${tot_rate} footage-hours per wall-hour"
python3 -c "
r=$tot_rate; h=$tot_h
print(f'  ETA to 200 h: {(200-h)/r:.1f} more wall-hours' if r > 0 else '  ETA: n/a')"
