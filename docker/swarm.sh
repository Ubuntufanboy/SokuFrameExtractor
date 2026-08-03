#!/usr/bin/env bash
# Launch/stop/inspect a container swarm on a host that has Docker.
#
#   docker/swarm.sh start 6      # 6 workers, shard i/6 each
#   docker/swarm.sh status
#   docker/swarm.sh stop
#
# runner/swarm.py does the same job with processes, for hosts that cannot run
# containers. This is the container version, and it is the right one wherever
# Docker exists: Wine is a large dependency to install on a machine somebody
# else uses, and here it lives entirely inside the image.
#
# WHY --user
# ----------
# The container would otherwise run as root, and Wine refuses to open a prefix
# it does not own ("wineserver: /wineprefix is not owned by you") when that
# prefix is a bind mount from a normal user's directory. Running as the
# invoking uid fixes it and has a second benefit: captures land on the host
# owned by that user rather than by root.
#
# WHY EVERY PATH IS A BIND MOUNT
# ------------------------------
# Docker volumes live under the daemon's data-root, which on a workstation is
# usually the system SSD. This was built for a host whose SSD had 10 GB free
# and whose HDD had 1.3 TB, so the image is the only thing allowed to land on
# the SSD; prefixes, corpus and captures all go where the space is.
set -euo pipefail

CMD="${1:-status}"
N="${2:-6}"

IMAGE="${SFE_IMAGE:-sfe-collect}"
ROOT="${SFE_ROOT:-/mnt/dataset}"
GAME="${SFE_GAME:-$ROOT/soku}"
CORPUS="${SFE_CORPUS:-$ROOT/corpus}"
OUT="${SFE_OUT:-$ROOT/out}"
PREFIXES="${SFE_PREFIXES:-$ROOT/prefixes}"
CPUS="${SFE_CPUS:-2}"
# 1g, not 2g. A worker's real footprint is ~700 MB-1 GB: the module's 150 MB
# ring, the game, Xvfb, and FFmpeg. 2g was picked as "comfortable headroom"
# and turned into a 12 GB ceiling on a 15 GB desktop.
MEM="${SFE_MEM:-1g}"
BUDGET_GB="${SFE_BUDGET_GB:-400}"
TIMEOUT="${SFE_TIMEOUT:-1800}"
CRF="${SFE_CRF:-26}"
NAME_PREFIX=sfe-w

case "$CMD" in
start)
    [ -d "$GAME" ] || { echo "no game at $GAME" >&2; exit 2; }
    ls "$CORPUS"/*.rep >/dev/null 2>&1 || { echo "no .rep in $CORPUS" >&2; exit 2; }

    # ---------------------------------------------------------------------
    # Memory is the binding constraint, not CPU. Check it.
    # ---------------------------------------------------------------------
    # This guard exists because its absence took a workstation off the
    # network. Six workers were launched at --memory 2g on a 15 GB machine
    # with ~5 GB already in use by the user's desktop: a 12 GB ceiling over
    # ~10 GB of headroom. Each worker really wants ~1-1.5 GB (Wine, the game,
    # Xvfb, FFmpeg, and the module's 150 MB ring), so the box went into swap
    # thrash and stopped answering SSH.
    #
    # CPU oversubscription degrades throughput; memory oversubscription kills
    # the host. They are not the same kind of mistake and must not be sized
    # the same way. MemAvailable is the kernel's own estimate of what can be
    # allocated without swapping, which is exactly the question here.
    avail_mb=$(awk '/^MemAvailable:/ {print int($2/1024)}' /proc/meminfo)
    mem_mb=$(python3 -c "
m='$MEM'.lower()
print(int(float(m[:-1]) * (1024 if m.endswith('g') else 1)))")
    want_mb=$((mem_mb * N))
    # Leave a quarter of what is available to the machine's actual owner.
    safe_mb=$((avail_mb * 3 / 4))
    echo "memory  : ${want_mb} MB requested, ${avail_mb} MB available"
    if [ "$want_mb" -gt "$safe_mb" ]; then
        max_n=$((safe_mb / mem_mb))
        echo >&2
        echo "REFUSING TO START: $N workers x $MEM = ${want_mb} MB, but only" >&2
        echo "${avail_mb} MB is available and we must not take more than" >&2
        echo "${safe_mb} MB of it. This is the check whose absence previously" >&2
        echo "took a workstation off the network." >&2
        echo >&2
        echo "Use at most $max_n worker(s) at ${MEM}, or lower SFE_MEM." >&2
        exit 2
    fi

    per=$(python3 -c "print(f'{$BUDGET_GB/$N:.3f}')")
    echo "workers : $N x ${CPUS} cpu, ${MEM} each"
    echo "corpus  : $(ls "$CORPUS"/*.rep | wc -l) replays"
    echo "budget  : ${BUDGET_GB} GB total (${per} GB per worker)"
    echo

    for i in $(seq 0 $((N - 1))); do
        mkdir -p "$PREFIXES/w$i" "$OUT/w$i"
        docker run -d --name "${NAME_PREFIX}$i" \
            --user "$(id -u):$(id -g)" -e HOME=/tmp \
            -e SFE_WORKERS="$N" \
            -v "$GAME:/game" \
            -v "$PREFIXES/w$i:/wineprefix" \
            -v "$OUT/w$i:/out" \
            -v "$CORPUS:/corpus:ro" \
            --cpus "$CPUS" --memory "$MEM" \
            --restart no \
            "$IMAGE" \
            --replay-dir /corpus --out /out \
            --shard "$i/$N" --max-output-gb "$per" \
            --timeout "$TIMEOUT" --crf "$CRF" --resume >/dev/null
        echo "  ${NAME_PREFIX}$i started"
    done
    echo
    echo "  docker/swarm.sh status"
    echo "  docker/swarm.sh stop"
    ;;

status)
    docker ps -a --filter "name=^${NAME_PREFIX}" \
        --format "  {{.Names}}  {{.Status}}" | sort
    echo
    python3 - "$OUT" <<'PY'
import json, os, sys, glob
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
hours = frames / 60 / 3600
print(f"  {ok} ok, {fail} failed, {hours:.2f} hours of footage, {vid/1e9:.1f} GB")
if hours:
    rate = vid / 1e6 / hours
    print(f"  {rate:.0f} MB per hour of footage "
          f"({'OK' if rate <= 700 else 'OVER'}, budget 700)")
PY
    ;;

stop)
    docker ps -q --filter "name=^${NAME_PREFIX}" | xargs -r docker stop -t 30 >/dev/null
    docker ps -aq --filter "name=^${NAME_PREFIX}" | xargs -r docker rm >/dev/null
    echo "stopped and removed all ${NAME_PREFIX}* containers"
    ;;

*)
    echo "usage: $0 {start N|status|stop}" >&2
    exit 2
    ;;
esac
