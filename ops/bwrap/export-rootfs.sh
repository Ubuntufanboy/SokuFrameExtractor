#!/usr/bin/env bash
# Build the collection image here, unpack it there. Run from the repo root on a
# machine that has Docker; the target needs neither Docker nor root.
#
#   ops/bwrap/export-rootfs.sh anon@192.168.1.130
#
# The tarball is never written to disk on either side -- `docker export` is
# piped straight into `tar -x` over ssh. That is not tidiness: the target this
# was written for had 7 GB free and the rootfs plus the game is 3.5 GB of it,
# so a staged copy would not have fit.
set -euo pipefail

TARGET="${1:?usage: export-rootfs.sh user@host [remote-dir]}"
REMOTE_DIR="${2:-sfe-rootfs}"
SSH_KEY="${SFE_SSH_KEY:-$HOME/.ssh/id_ed25519_ai}"
IMAGE="${SFE_IMAGE:-sfe-collect}"

cd "$(dirname "$0")/../.."

echo "==> building $IMAGE"
docker build -f docker/collect.Dockerfile -t "$IMAGE" . >/dev/null

# `docker create` without starting: we want the filesystem, not a run.
cid=$(docker create "$IMAGE")
trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT

# What is dropped, and why each is safe:
#
#   dev/*       tar cannot mknod unprivileged and would fail the extraction.
#               bwrap mounts its own /dev anyway.
#   proc/*, sys/*   kernel filesystems; bwrap mounts those too.
#   usr/share/doc, man, info, locale   ~200 MB of text nothing reads headless.
#   var/cache/*, var/lib/apt/lists/*   apt metadata; there is no network use
#               inside the sandbox and no package will be installed.
#
# Wine's own locale data lives in usr/lib/i386-linux-gnu/wine and share/wine,
# neither of which is touched here -- dropping those breaks the game.
echo "==> streaming rootfs to $TARGET:~/$REMOTE_DIR"
docker export "$cid" \
  | ssh -i "$SSH_KEY" -o Compression=no "$TARGET" \
        "mkdir -p ~/$REMOTE_DIR && tar -x -C ~/$REMOTE_DIR \
            --no-same-owner --no-same-permissions \
            --warning=no-unknown-keyword \
            --exclude='dev/*' --exclude='proc/*' --exclude='sys/*' \
            --exclude='usr/share/doc/*' --exclude='usr/share/man/*' \
            --exclude='usr/share/info/*' --exclude='usr/share/locale/*' \
            --exclude='var/cache/*' --exclude='var/lib/apt/lists/*' \
        || true"

# Two fixups the extraction cannot do by itself.
#
# 1. The rootfs has only root in /etc/passwd, but the sandbox runs as the
#    invoking uid. Wine calls getpwuid() during prefix creation and a failed
#    lookup surfaces much later as an unexplained wineboot failure.
# 2. /tmp, /out and friends are mounted over at runtime, but the mount points
#    have to exist in the image first.
echo "==> fixing up the rootfs"
ssh -i "$SSH_KEY" "$TARGET" "
    set -e
    cd ~/$REMOTE_DIR
    mkdir -p app game wineprefix out replays tmp run dev proc sys
    id=\$(id -u); gid=\$(id -g); name=\$(id -un)
    grep -q \"^\$name:\" etc/passwd || \
        echo \"\$name:x:\$id:\$gid::/wineprefix:/bin/bash\" >> etc/passwd
    grep -q \"^\$name:\" etc/group || echo \"\$name:x:\$gid:\" >> etc/group
    du -sh . | sed 's/^/rootfs: /'
"
echo "==> done"
