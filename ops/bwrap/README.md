# Running collection where you are not root

`docker/collect.Dockerfile` is the supported way to capture, and it needs a
Docker daemon. On a machine where you cannot have one, these scripts run the
*same* Debian bookworm filesystem under `bwrap`, unprivileged.

That is not a preference. The image exists because the extractor patches 32-bit
code inline and Wine's new-WoW64 path — which is all Ubuntu 24.04+ and Debian
trixie ship — crashes the game during module load
(`C0000005`, before the module logs a line). Bookworm's `wine32:i386` is a
classic WoW64 build with real i386 libraries, and it is the only environment
this has ever worked in. So the rootfs is not repackaged or rebuilt from the
host distribution: it is `docker export` of the image, byte for byte.

## Why bwrap and not rootless Docker or podman

Both want `newuidmap`/`newgidmap`, which are setuid binaries from the `uidmap`
package — installing them needs root, which is the thing we do not have.
`bwrap` is already present on Ubuntu and is permitted to create a user
namespace by the shipped `bwrap-userns-restrict` AppArmor profile, even with
`kernel.apparmor_restrict_unprivileged_userns=1`. Check with:

    bwrap --ro-bind / / --proc /proc --dev /dev --tmpfs /tmp echo ok

If that prints `ok`, everything here works.

## Usage

Verified on 192.168.1.130 (Ubuntu 26.04, no root, no Docker): 14 captures,
~157 000 frames, every one passing `pipeline.validate`, `pipeline.verify` and
all 13 checks in `pipeline.verify_state`.

## Throughput, measured on a Ryzen 5 4500 (6 cores / 12 threads)

| workers | aggregate | per replay |
|---|---|---|
| 1 x 6 cores | 135 fps (2.3x real time) | 66 380 frames in 490 s |
| 2 x 6 cores | 183 fps (3.0x real time) | 90 336 frames in 495 s |

A second worker buys **1.35x, not 2x**: six physical cores are already
saturated by llvmpipe and x264, and the second worker mostly fills SMT
siblings. Worth having, not worth planning around.

Captures average 34 MB and 11 000 frames. So re-capturing the 2003-replay
corpus is about **34 hours and 67 GB** at two workers — which is why it cannot
happen on this box (1.3 GB free) without shard-and-upload.

**Capture is not bit-deterministic.** Two runs of replay 5262777 produced
10 073 and 10 074 frames. The CSV and video from *one* run always agree, so
this costs nothing normally -- but it rules out the tempting shortcut of
keeping the existing corpus videos and pairing them with freshly captured
sidecars, because the pairing would be silently off by a frame.

## Usage

On a machine with Docker, build and ship the rootfs:

    ops/bwrap/export-rootfs.sh anon@192.168.1.130

Then ship the game and the code:

    ops/bwrap/stage-game.sh anon@192.168.1.130 /path/to/Soku

Then, on the target:

    ~/sfe/ops/bwrap/sfe-bwrap python3 -m runner.collect \
        --prefix /wineprefix --replay-dir /replays --out /out --limit 1

## What the sandbox is

`sfe-bwrap` binds the exported rootfs over `/` and mounts four writable paths
into it, so nothing on the host is visible except what capture needs:

| inside      | outside (env)          | why                                  |
|-------------|------------------------|--------------------------------------|
| `/app`      | `~/sfe` (`SFE_APP`)    | the runner and pipeline code         |
| `/game`     | `~/sfe-game` (`SFE_GAME`) | the game install (written to: ini) |
| `/wineprefix` | `~/sfe-prefix` (`SFE_PREFIX`) | the win32 prefix, made on first run |
| `/replays`  | `~/sfe-replays` (`SFE_REPLAYS`) | `.rep` corpus, read-only    |
| `/out`      | `~/sfe-out` (`SFE_OUT`) | captures                            |

`/tmp` is a tmpfs inside the sandbox, which is where the X socket goes. The
FIFO does **not** live there — it goes to `/out/.work`, on real disk, because
`runner/collect.py` refuses a tmpfs output for the reason in its `is_tmpfs`
docstring.

## Running more than one at a time

Every one of `SFE_GAME`, `SFE_PREFIX` and `SFE_OUT` must differ per worker, and
`SFE_CPU_BLOCK` should, so the workers pin to disjoint cores
(`runner/throttle.py`). The game directory is the one that is easy to miss:

    ops/bwrap/clone-game.sh ~/sfe-game ~/sfe-game1     # ~22 MB, .dat hardlinked

    for i in 0 1; do
      g=$HOME/sfe-game; [ $i -gt 0 ] && g=$HOME/sfe-game$i
      SFE_GAME=$g SFE_PREFIX=$HOME/sfe-prefix$i SFE_OUT=$HOME/sfe-out$i \
      SFE_CPU_BLOCK=$i ops/bwrap/sfe-bwrap /app/docker/entrypoint.sh \
          --replay-dir /replays --out /out --shard $i/2 --cpus 6 &
    done; wait

Sharing one game directory looks like it works and does not. `collect.py`
writes a per-job `SokuFrameExtractor.ini` into `modules/SokuFrameExtractor/`
naming that job's FIFO and output directory, so two workers overwrite each
other's between launches. Measured here: worker 1 completed six replays while
worker 0 hung on its first, its game happily writing into worker 1's FIFO
while worker 0's FFmpeg waited on a pipe nobody would ever open. Neither log
said anything until the 900-second timeout. `runner/swarm.py` avoids this by
cloning the game into each prefix; `sfe-bwrap` binds one directory in, so the
clone happens outside.

**The manifest records sandbox paths.** `manifest.jsonl` stores `/out/<id>/
video.mp4`, which does not exist on the host, so `python3 -m pipeline.verify`
run outside reports every capture as missing. Run it through `sfe-bwrap` and it
resolves. The same is true of the Docker path and is not new; it is just easy
to trip over when the host shell can see the files at a different path.

A PID namespace is used so that a wedged `wineserver` cannot outlive the
sandbox. That matters more here than in Docker: on a shared box a stranded
game process keeps rendering on the CPU forever, and the usual teardown
(`wineserver -k`) is exactly what fails when it is wedged.
