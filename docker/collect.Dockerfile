# ---------------------------------------------------------------------------
# Headless Soku collection worker.
# ---------------------------------------------------------------------------
# Runs the 32-bit game under Wine + Xvfb with Mesa's llvmpipe software
# rasteriser, captures frames through the SokuFrameExtractor module, and pipes
# raw BGRA to FFmpeg over a FIFO.
#
# The game is MOUNTED, never baked in: it is copyrighted and ~2 GB.
#     docker run --rm \
#       -v /path/to/Soku:/game \
#       -v "$PWD/out:/out" \
#       -v sfe-prefix:/wineprefix \
#       --cpus 4 --memory 3g \
#       sfe-collect --limit 3
#
# NO INPUT DEVICES ARE NEEDED.
# Earlier revisions carried openbox, xdotool and python3-evdev, and demanded
# --device=/dev/uinput, to drive the menus by synthesising keystrokes. None of
# it ever worked: Wine's DirectInput reads real evdev devices, so nothing
# injected above the kernel reaches the game. The module now starts a replay by
# calling what the replay menu's confirm handler calls -- readReplay() and
# setBattleMode() -- from inside a hook on the current scene's onProcess. All
# of that machinery is gone from this image.
#
# Deliberately uses Debian's Wine rather than WineHQ's newest: bookworm ships a
# classic 32-bit Wine build with real i386 libraries. Wine 11's new-WoW64 path
# runs 32-bit code through a 64-bit host process, and its 32-bit GL support is
# markedly less well trodden -- which is exactly the surface this image depends
# on (wglSwapBuffers -> wined3d -> OpenGL).
#
# Kept deliberately small. This is built on hosts whose Docker root shares a
# nearly-full system SSD, where every avoidable gigabyte is a gigabyte of
# somebody's working disk.
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim

# 32-bit runtime is mandatory: th123.exe is an i386 binary and the GL stack it
# loads must match.
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        wine \
        wine32:i386 \
        libwine:i386 \
        xvfb \
        xauth \
        libgl1-mesa-dri:i386 \
        libglx-mesa0:i386 \
        libgl1:i386 \
        ffmpeg \
        python3 \
        procps \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && apt-get clean

# NO winetricks, and no MSVC redistributable.
#
# This image used to fetch winetricks to install vcrun2019, because Wine's
# builtin msvcp140.dll aborts the process during module load -- it does not
# implement ?_Throw_Cpp_error@std@@YAXH@Z, which std::mutex and std::thread
# reach. The module no longer links the MSVC STL at all (it uses Win32
# primitives directly), so the dependency is gone, and
# cmake/CheckNoMsvcp.cmake fails the build if it ever returns.

# Software rendering, stated explicitly rather than left incidental: Xvfb
# exposes no DRI device so Mesa would fall back to llvmpipe anyway, but saying
# so keeps the image correct if /dev/dri is ever mounted for VAAPI encoding --
# we want the GPU for encode, not for render, because the game's GL output has
# to land in a PBO we can read back cheaply.
#
# The llvmpipe tuning that actually matters -- thread count and vector width --
# is set per process by runner/wine.py, which sizes it against the CPU budget
# the container was actually given rather than against the host's core count.
# Getting that wrong cost a 1.25x throughput factor; see runner/throttle.py.
ENV LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=llvmpipe \
    WINEDEBUG=-all \
    PULSE_SERVER=/nonexistent-sfe \
    PYTHONUNBUFFERED=1

# No PulseAudio socket and no /dev/snd inside the container, so audio is silent
# by construction here. The runner still sets the Wine audio overrides because
# it also runs directly on workstations, where it is not.

WORKDIR /app
COPY runner/ /app/runner/
COPY pipeline/ /app/pipeline/
COPY config/ /app/config/
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# The prefix is created by the entrypoint (WINEARCH=win32) rather than reused
# from the host: a win64 prefix cannot be opened by a 32-bit wineserver, and on
# a new-WoW64 Wine it crashes the module during load. Mount a volume here to
# avoid re-running wineboot on every launch.
VOLUME ["/wineprefix"]
ENV WINEPREFIX=/wineprefix GAME_DIR=/game

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["--help"]
