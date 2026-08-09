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

| inside      | outside                | why                                  |
|-------------|------------------------|--------------------------------------|
| `/app`      | `~/sfe`                | the runner and pipeline code         |
| `/game`     | `~/sfe-game`           | the game install (written to: ini)   |
| `/wineprefix` | `~/sfe-prefix`       | the win32 prefix, made on first run  |
| `/replays`  | `~/sfe-replays`        | `.rep` corpus, read-only             |
| `/out`      | `~/sfe-out`            | captures                             |

`/tmp` is a tmpfs inside the sandbox, which is where the X socket goes. The
FIFO does **not** live there — it goes to `/out/.work`, on real disk, because
`runner/collect.py` refuses a tmpfs output for the reason in its `is_tmpfs`
docstring.

**The manifest records sandbox paths.** `manifest.jsonl` stores `/out/<id>/
video.mp4`, which does not exist on the host, so `python3 -m pipeline.verify`
run outside reports every capture as missing. Run it through `sfe-bwrap` and it
resolves. The same is true of the Docker path and is not new; it is just easy
to trip over when the host shell can see the files at a different path.

A PID namespace is used so that a wedged `wineserver` cannot outlive the
sandbox. That matters more here than in Docker: on a shared box a stranded
game process keeps rendering on the CPU forever, and the usual teardown
(`wineserver -k`) is exactly what fails when it is wedged.
