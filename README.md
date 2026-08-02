# SokuFrameExtractor

<p align="center">
  <img src="img/gameplay.png" alt="Touhou Hisoutensoku gameplay" width="640">
</p>

<p align="center">
  <b>Frame-accurate gameplay capture for Touhou 12.3 (Hisoutensoku).</b>
  <br>
  Records what the game rendered and what both players pressed, one row per frame.
</p>

<p align="center">
  <video src="docs/assets/showcase.mp4" width="640" controls></video>
  <br>
  <a href="docs/assets/showcase.mp4"><b>▶ showcase.mp4</b></a> — captured frames with the
  recorded inputs drawn back over them.
  <br>
  <sub>Each lit key is read from that frame's row in <code>inputs.csv</code>. If the
  overlay and the action ever drifted apart, it would be visible here — which is
  exactly why the overlay exists.</sub>
</p>

---

## What this is

A SWRSToys module that hooks the game's OpenGL present path, reads back each
frame via asynchronous PBO transfers, and streams raw BGRA into FFmpeg through
a FIFO — while recording both players' inputs for the same frame. The result is
a video plus a CSV where **row *i* describes video frame *i***.

It is data-collection infrastructure for a world model that plays Soku from
natural-language instructions. The model itself lives in a separate repo; this
one produces its training data.

### Output

```
out/<replay-stem>-<sha8>/
  video.mp4     # 640x480 H.264, 60 fps
  inputs.csv    # one row per video frame
  ffmpeg.log
  wine.log
out/manifest.jsonl
```

`inputs.csv` columns:

| column | meaning |
|---|---|
| `frame` | dense row counter — this is what video frame *N* maps to |
| `game_frame` | engine tick the frame came from (may repeat if the game presents twice per tick) |
| `p1_input` / `p2_input` | raw 10-bit mask |
| `p1_up` … `p2_spell` | the same mask expanded to 20 boolean columns |

Input bits: `0x001` up, `0x002` down, `0x004` left, `0x008` right, `0x010` A
(melee), `0x020` B (weak bullet), `0x040` C (strong bullet), `0x080` D (dash),
`0x100` change card, `0x200` spell.

Directory names are keyed on the source `.rep`'s content hash, so every capture
is attributable to the exact file that produced it.

Budget roughly **100 MB per replay** at the default `--crf 23` — a measured
5-minute match came to 118 MB for 18,144 frames. The whole 397-replay corpus is
therefore tens of GB, not the few GB a smaller sample suggests; raise `--crf`
if that matters more than fidelity does.

---

## Status

**Working and verified**

- Frame capture: `wglSwapBuffers` hook, 3-deep PBO ring, ~600 MB/s of BGRA into
  a FIFO. Sustained 58–238 fps with the frame limiter removed.
- Input capture frame-aligned with the video, 1:1 by construction (both are
  written from the same ring slot, on the same thread).
- Reproducible cross-build of the DLL from Linux (MSVC via msvc-wine).
- `pipeline/validate.py` integrity gate, regression-tested against known-bad
  captures.
- One replay per game process, with machine-readable status and a real exit
  code.
- Headless rendering under Xvfb + Mesa llvmpipe (verified by screenshot).
- **Unattended replay start** — no menus, no keypresses, no human. See below.
- Corpus fan-out: `--shard I/N` splits the replay list into disjoint, complete
  strides, one per container or `runner/swarm.py` worker.
- Corpus acquisition from sokureplays.delthas.fr, with ground-truth matchup
  metadata (characters, decks, Elo, outcome).
- Square 480x480 output inside a hard bitrate budget: **599 MB per hour of
  footage** measured over six captures, against a 700 MB/h ceiling.
- Recovery of legacy BMP captures into the modern layout
  (`pipeline/from_bmp.py`), and the showcase clip above, built from real
  recorded gameplay.

**Working: starting a replay with no menus and no keypresses**

The module does what the buttons do, instead of pressing them. Read off
`th123.exe`, the replay list's confirm handler (`0x0044B3D0`) and the menu
scene's scene-6 case (`0x00427786`) are together just:

```c
readReplay(path)                                  // 0x0042EAC0, this = 0x00898718
mode = *(uint8_t *)(0x00898718 + 0xEC)            // battle mode from the .rep header
setBattleMode(mode == 0 ? 0 : mode == 7 ? 7 : 3, 2)   // 0x0043E9A0
return SCENE_LOADING                              // 6
```

That has to run **inside a hook on the current scene's `onProcess`** (vtable
slot 1), because the engine's main loop asks for the next scene by calling it:

```c
if (sceneId == newSceneId) {                          // 0x8A0044 vs 0x8A0040
    newSceneId = scene->onProcess();                  // scene object at 0x8A000C
    if (newSceneId != sceneId) CreateThread(loadGraphics);
} else if (!loadingScreen->isBusy()) commitSceneSwap();
```

Same thread, same call site, same return path a keypress would have taken.
`readReplay` returning false means the game itself rejected the file, which is
a much better error than "navigation failed".

Two things had to be right, and both had already cost a day when they were
guessed at instead:

- **`setBattleMode` is not two byte stores.** It initialises about twenty
  globals, one of which (`0x00899D08`) is the *argument* to the battle worker
  task that used to fault at `0x004386A6`. Writing `mainMode`/`subMode` by hand
  and leaving the rest stale is exactly what the battle creator crashed on.
- **`setBattleMode` faults in a container with no input devices.** It does
  `deviceProfiles.at(selectedDevice)`; `deviceProfiles` (`0x00899CEC`) is built
  by enumerating DirectInput devices, and the checked `.at()` raises
  `0xC000000D` when the index is past the end. A container has no
  `/dev/input`, so the vector is **empty** and `.at(0)` kills the process. The
  module sets the selector to the game's own "no device" value (`-2`) when, and
  only when, the index would be out of range — replay inputs come from the
  `.rep` stream, so no device is ever read.

That second one is the same root cause as the input problem below, surfacing
somewhere else entirely: not "keys do not arrive" but "the device list is
empty".

**Not working, and no longer needed: synthetic keyboard input**

Soku reads the keyboard through DirectInput and under Wine nothing synthetic
reaches it. Seven approaches were tried and all failed identically — the game
renders normally and the scene id never moves. None of them are on the path
any more; the table is kept so nobody spends another day on it.

| approach | layer | result |
|---|---|---|
| `SendInput` | Win32 | swallowed |
| `SendInput` + `SetForegroundWindow`/`SetFocus` | Win32 | swallowed |
| `PostMessage(WM_KEYDOWN/WM_KEYUP)` | Win32 | swallowed |
| `SendInput` + a window manager for focus | Win32 | swallowed |
| `xdotool` / XTEST | X server | swallowed |
| `/dev/uinput` virtual keyboard | kernel | device is global — types into the host's desktop; unavailable in an unprivileged container |
| hooking `checkKeyOneshot` (`0x0043DE30`) | game code | hook installs, but the game never calls it — 0 invocations logged |

The root cause is that Wine's dinput reads real evdev devices. Every approach
above injects above the kernel, so dinput never sees any of it.

**Not built yet**

- `.npz` shard export for the training repo.
- Validated matchup metadata — see `pipeline/REPPARSE_STATUS.md`.
- A full corpus run to completion. The mechanism works and shards across
  workers; throughput on the current box is bounded by a 5.76-core cgroup
  quota, so a 52 GB target is roughly 40 hours of wall clock.

---

## Quick start

The game is **not** included and is never baked into an image; point the tools
at your own installation.

```bash
# 1. Build the module (needs msvc-wine; see Building)
cmake --preset msvc-wine
cmake --build --preset msvc-wine
cp build/msvc-wine/dll/SokuFrameExtractor.dll \
   "$WINEPREFIX/drive_c/Games/Soku/modules/SokuFrameExtractor/"

# 2. Collect (in the container — see Known issues for why)
docker build -f docker/collect.Dockerfile -t sfe-collect .
docker run --rm --cpus 2 --memory 4g \
  -v "$HOME/.wine-soku:/prefix" \
  -v "$PWD/out:/out" \
  sfe-collect --prefix /prefix --out /out --limit 3

# 3. Check what you got
python3 -m pipeline.validate out/

# 4. Look at it — overlay the inputs onto the video
python3 -m pipeline.overlay out/<capture>/

# 5. Build the showcase reel
python3 -m pipeline.trailer out/ -o docs/assets/showcase.mp4
```

### Building a corpus from sokureplays.delthas.fr

```bash
python3 -m pipeline.fetch_replays -o corpus -n 2600
```

Pages the site's JSON backend and downloads each `.rep`, writing an
`index.jsonl` of listing metadata beside them. That metadata is the point as
much as the files are: it carries **characters, both 20-card decks, Elo,
rounds and the winner**, which is ground truth for the conditioning signal the
local `.rep` header parser could never be trusted to produce (see
`pipeline/REPPARSE_STATUS.md`).

The fetcher is deliberately unhurried — one request at a time, paced, backing
off on errors, identifying itself, and fully resumable. Player nicknames are
stripped by default; the corpus needs characters and decks, not identities.

### Fanning a corpus out across workers

`--shard I/N` gives each container a disjoint slice of the corpus. The split is
a pure function of the sorted replay list, so the containers need no
coordination and never collide:

On a host with Docker, one container per shard:

```bash
# 20 replays, 10 containers -> 2 each
for i in $(seq 0 9); do
  docker run -d --cpus 2 --memory 4g \
    -v "$HOME/.wine-soku:/prefix:ro" -v "$PWD/out/shard$i:/out" \
    sfe-collect --prefix /prefix --out /out --shard "$i/10"
done
```

Without Docker — which includes any host that *is* already an unprivileged
container — `runner/swarm.py` does the same thing with processes:

```bash
python3 -m runner.swarm start --workers 4 --prefix ~/.wine-soku \
    --corpus corpus --out dataset --budget-gb 52
python3 -m runner.swarm status --out dataset
python3 -m runner.swarm stop   --out dataset
```

It clones the Wine prefix per worker (hardlinking the 2.1 GB of read-only
`.dat` archives, so ten prefixes cost ~6.7 GB rather than ~26), partitions the
X display numbers, and runs each worker detached with `--shard i/N`.

**Size the swarm against the real CPU budget, not `nproc`.** See
[Known issues](#a-cgroup-cpu-quota-makes-nproc-a-lie); `swarm start` prints the
quota and warns when you oversubscribe it.

Each container plays its replays one at a time, one game process each, and
writes a `video.mp4` + `inputs.csv` pair per replay. Merge the shards by
concatenating their `manifest.jsonl` files; capture directories are keyed on
the source `.rep`'s content hash, so names cannot collide.

Slices are **strided, not contiguous**: replays sort by filename and therefore
cluster by date and matchup, so a contiguous block would hand one container a
whole tournament. Striding gives every shard the same mix, which matters
because a partial corpus is the normal outcome of a fan-out run.

One Wine prefix serves one container. Give each its own prefix volume, or
mount a shared one read-only — `runner/collect.py` takes an exclusive `flock`
on the prefix and refuses to start a second collector against it, because two
collectors sharing a wineserver silently produce garbage rather than an error.

### Recovering older BMP-era captures

Captures from before the video pipeline wrote one BMP per frame plus an
`inputs.csv` with a `frame_file` column. Their `frame_index` is unreliable, but
`frame_file` is not, so the row-to-frame pairing survives:

```bash
python3 -m pipeline.from_bmp <old-capture>/ -o out/recovered
```

It sorts by `frame_file` to restore true order, selects the longest run of
contiguous frames (older captures dropped many), and emits the modern
`video.mp4` + `inputs.csv`. The showcase above was produced this way.

### Running on a machine you are using

Collection is CPU-hungry: llvmpipe rasterises in software and x264 encodes
beside it. `--cpus` (default: half your cores, capped at 4) pins the run to the
highest-numbered cores at `nice 19`, which keeps a desktop responsive. Scale out
with more containers rather than more cores per job.

`--out` must be on real storage. The runner refuses to write to `tmpfs`
(`/tmp` on most distributions) — a full corpus is several GB and would be
backed by RAM.

---

## Building

**MSVC is required. MinGW is not a supported alternative.** SokuLib binds the
game's C++ objects through hardcoded vtable indices and
`__thiscall`/`__fastcall` member pointers, which are MSVC ABI details. A MinGW
build links cleanly and then corrupts the game at runtime, so `CMakeLists.txt`
refuses to configure with anything else.

The supported toolchain is [msvc-wine](https://github.com/mstorsjo/msvc-wine),
which runs the real MSVC compiler under Wine. Note that its installer downloads
the MSVC toolchain from Microsoft under the Visual Studio licence, so the build
image is for local use and must not be pushed to a public registry.

```bash
git submodule update --init --recursive     # pins SokuLib
source scripts/msvc-env.sh                  # puts cl/link/rc on PATH
cmake --preset msvc-wine
cmake --build --preset msvc-wine
```

Verify the result exports what SWRSToys resolves by name:

```bash
i686-w64-mingw32-objdump -p build/msvc-wine/dll/SokuFrameExtractor.dll | grep -A4 'Ordinal/Name'
# CheckVersion, Initialize   (pei-i386)
```

---

## Configuration

`config/sfe.ini` is the single source of truth, installed next to the DLL as
`SokuFrameExtractor.ini`. The runner rewrites it per job.

| key | meaning |
|---|---|
| `ReplayDir` | directory holding the **one** staged `.rep` |
| `OutputDir` | where `inputs.csv` is written |
| `FifoPath` | video sink; `Z:\` maps to the Linux filesystem root |
| `StatusPath` | where the DLL writes its machine-readable result |
| `FastForward` | remove the 60 fps limiter while capturing |
| `Verbose` | per-frame FSM logging |

`tests/test_config_parity.py` fails if this file, `loadConfig()`, and
`struct Config` ever disagree. They had drifted badly before: the shipped
`.ini` advertised three options no code read, while the loader read two that
appeared in no `.ini`.

---

## Design notes

**One replay per game process.** The runner stages exactly one `.rep`, launches
the game, and waits for it to exit. This is what makes replay identity exact
(the game can only see one file), isolates crashes to a single replay, and
makes parallelism a matter of running more containers.

The earlier design walked a 397-entry list inside one long-lived process and
named output from a counter. Two defects followed from that shape, both
recoverable from the shipped output:

- Capture stayed armed between replays, so menus and result screens were
  recorded under a frozen gameplay frame index. One file repeats frame 8622
  **31,789 times** — 79% of its rows.
- The per-replay CSV was swapped on the game thread while the encoder thread
  was still draining the previous replay, so each file begins with its
  predecessor's tail. Each CSV starts at exactly the frame its predecessor got
  stuck on: 9718 → 8622 → 7696, three consecutive files.

`pipeline/validate.py` exists so that class of corruption cannot pass silently
again; it is regression-tested against those files.

**The input overlay is a QA instrument.** Every automated check is structural —
row counts match, indices increase, the video decodes. None can tell you
whether the inputs on frame *N* produced the action visible on frame *N*.
Watching a button light up as a character swings can, which is why
`pipeline/overlay.py` is part of the pipeline rather than a presentation
afterthought.

---

## Known issues

### The module crashes at load under new-WoW64 Wine

Wine builds without `/usr/lib/wine/i386-unix/` run 32-bit Windows code thunked
through a 64-bit host process. The extractor patches 32-bit code inline — a
5-byte JMP over `wglSwapBuffers` and a BattleManager vtable overwrite — which is
exactly the surface that changes. The game dies with `C0000005` before the
module logs anything.

Check with:

```bash
ls -d /usr/lib/wine/i386-unix    # missing => new-WoW64 only
```

Use `docker/collect.Dockerfile`, which installs Debian bookworm's `wine32:i386`
— a classic WoW64 build with real 32-bit libraries. The container is
load-bearing for correctness here, not just reproducibility.

### Wine's builtin `msvcp140.dll` aborts the process

A separate failure, and the one that bites in a *fresh* prefix. The module is
built with MSVC and uses `std::thread`/`std::mutex`, whose failure paths call
`?_Throw_Cpp_error@std@@YAXH@Z`. Wine's builtin `msvcp140.dll` does not
implement it, and Wine aborts the moment it is called:

```
wine: Call from ... to unimplemented function
      msvcp140.dll.?_Throw_Cpp_error@std@@YAXH@Z, aborting
```

The symptom is misleading: `SokuFrameExtractor.log` exists but is **0 bytes** —
`initLog()`'s `fopen` succeeded and the abort landed before the first log line
was written. That looks exactly like "the module never loaded", but it did.
Check `wine.log` and `/proc/<pid>/maps` before concluding otherwise.

`docker/entrypoint.sh` installs the real runtime (`winetricks -q vcrun2019`,
network needed on first run only) and sets `msvcp140=n,b`. Set
`SFE_SKIP_VCRUN=1` to skip it if you provide the DLLs yourself.

The durable fix is to drop the MSVC C++ STL from the module — Win32
`CreateThread` / `CRITICAL_SECTION` / `CONDITION_VARIABLE` instead of
`std::thread` / `std::mutex` — which removes the redistributable dependency
entirely. Not done yet.

### A crash during module load disables the module behind a modal dialog

SWRSToys records the module it is loading in `currentModule.txt` and clears the
file on success. If the game dies in between, the next launch disables that
module and shows a dialog:

> SokuFrameExtractor.dll has been disabled because the game crashed while
> loading it last time.

Headless, nobody clicks OK, so the game waits at the dialog forever — one crash
turns an unattended run into a series of identical timeouts, with empty logs
because the module never ran. `runner.wine.clear_crash_sentinel()` clears this
before every launch.

### `Initialize()` runs under the Windows loader lock

SWRSToys calls it from inside its own `DllMain`. Do not create threads there —
the new thread's `DLL_THREAD_ATTACH` needs the lock you are holding, and the
game crashes during module load. `VideoEncoder::start()` is deliberately
deferred to the first `onFrame()`, on the game thread.

### Synthetic keyboard input does not reach the game under Wine (worked around)

Soku reads the keyboard through DirectInput, and **Wine's dinput reads real
evdev devices under `/dev/input`** — not Wine's synthetic Win32 input queue,
and not the X server's synthetic events. Everything that injects above the
kernel is therefore invisible to it, on a desktop or in a container. See
[Status](#status) for the full table of what was tried.

This also explains why a container is strictly *worse* than a desktop rather
than merely equal: a container has no `/dev/input` at all, so dinput has no
keyboard to enumerate under any circumstances.

Two consequences, both handled:

- Menu navigation is gone. The module calls what the menu handler calls
  instead — see [Status](#status).
- `setBattleMode` indexes the (empty) DirectInput device list and crashes.
  `session.cpp` selects the game's own "no device" profile when the index
  would be out of range.

`runner/menu.py` and `runner/vkbd.py` are kept as documented dead ends, not as
part of the pipeline. Nothing calls them.

### A cgroup CPU quota makes `nproc` a lie

The GPU box reports 48 CPUs and is capped at **5.76**:

```
$ nproc
48
$ cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us   # 576000
$ cat /sys/fs/cgroup/cpu/cpu.cfs_period_us  # 100000
```

Every core-count decision sized from `os.cpu_count()` is therefore wrong by
8x inside a container, and the failure is not graceful. Ten workers at four
cores each — 40 "cores" of a 48-thread machine — captured at 5 fps apiece,
roughly *half the total throughput of a single worker*, because each one paid
the full per-frame cost of a game process for a twentieth of a core.

The diagnostic that gives it away is a contradiction:

```
$ vmstat 2
 r  b ...  us sy id
78  0 ...  12  9 80      <- 78 runnable tasks AND 80% idle
```

The idle is the host's; the run queue is ours.

`runner/throttle.py` reads the quota (v2 `cpu.max`, v1 `cfs_quota_us`) and
`swarm start` warns when `workers x cpus` exceeds it.

**Under a quota, `taskset` is actively harmful.** Pinning exists to keep a
run from monopolising a machine somebody is using; a quota already does that,
and does it better, because it caps total CPU while leaving the scheduler free
to place threads wherever there is room. `taskset` on top caps nothing further
— it only forbids the scheduler from using idle cores. Three workers pinned
two cores each ran 48/52/5 fps, the third starving behind a neighbour. So
pinning is skipped whenever a quota is present.

### Xvfb rejects depth 32

`-screen 0 640x480x32` fails with "Couldn't add screen 0". Use depth 24; Mesa
still provides llvmpipe GL 4.5, well beyond the GL 2.1-era PBOs this needs.

---

## Repository layout

```
dll/          C++ SWRSToys module (hook, ring buffer, encoder, FSM)
runner/       host orchestration: staging, FIFO/FFmpeg, Wine, CPU budget
pipeline/     dataset tooling: validation, manifest, overlay, .rep parsing
docker/       build and collection images
toolchains/   msvc-wine CMake toolchain
tests/        config parity and pipeline tests
config/       sfe.ini — the single source of truth
```

---

## Licence

See [LICENSE](LICENSE). The game itself is not distributed here and is not
covered by it.
