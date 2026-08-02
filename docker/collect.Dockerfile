# ---------------------------------------------------------------------------
# Headless Soku collection worker.
# ---------------------------------------------------------------------------
# Runs the 32-bit game under Wine + Xvfb with Mesa's llvmpipe software
# rasteriser, captures frames through the SokuFrameExtractor module, and pipes
# raw BGRA to FFmpeg over a FIFO.
#
# The game is MOUNTED, never baked in: it is copyrighted and ~5 GB.
#     docker run --rm \
#       --device=/dev/uinput \
#       -v /path/to/Soku:/game \
#       -v "$PWD/out:/out" \
#       -v sfe-prefix:/wineprefix \
#       --cpus 2 --memory 4g \
#       sfe-collect --limit 3
#
# --device=/dev/uinput is REQUIRED. Menu navigation drives a virtual keyboard
# created through uinput, because Wine's DirectInput reads real evdev devices
# and ignores every form of synthetic input above the kernel. See runner/vkbd.py.
# The host must have the uinput module loaded (modprobe uinput).
#
# Deliberately uses Debian's Wine rather than WineHQ's newest: bookworm ships a
# classic 32-bit Wine build with real i386 libraries. Wine 11's new-WoW64 path
# runs 32-bit code through a 64-bit host process and its 32-bit GL support is
# markedly less well trodden, which is exactly the surface this image depends
# on (wglSwapBuffers -> wined3d -> OpenGL).
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim

# 32-bit runtime is mandatory: th123.exe is an i386 binary and the GL stack it
# loads must match.
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        wine \
        wine32:i386 \
        cabextract \
        wget \
        libwine:i386 \
        xvfb \
        xauth \
        x11-utils \
        libgl1-mesa-dri:i386 \
        libglx-mesa0:i386 \
        libgl1:i386 \
        mesa-utils \
        ffmpeg \
        python3 \
        procps \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Small, frequently-adjusted tools go in their own layer. Adding a package to
# the big apt block above invalidates it and costs a full ~15 minute rebuild of
# the whole Wine/Mesa/FFmpeg dependency tree; adding one here costs seconds.
#
#   openbox  -- a window manager, so a window can actually hold focus. Bare
#               Xvfb assigns focus to nothing.
#   xdotool  -- XTEST key injection. Menu navigation runs from the host side
#               through this; see runner/menu.py for why in-process input
#               injection cannot work under Wine.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
# Small, frequently-changed tools -- deliberately a SEPARATE layer.
# ---------------------------------------------------------------------------
# Adding a package to the big apt block above invalidates it and rebuilds the
# entire Wine/Mesa/FFmpeg dependency tree: ~15 minutes and a few GB of churn.
# Adding one here costs seconds. This split was learned the hard way -- the
# tools below were each added one at a time during debugging, and every one
# triggered a full rebuild.
#
#   openbox        a window manager, so a window can hold focus at all. Bare
#                  Xvfb assigns focus to nothing.
#   xdotool        window lookup/activation (XTEST key injection is kept for
#                  focus handling; it does NOT drive the game -- see below).
#   python3-evdev  creates the uinput virtual keyboard that actually drives
#                  the menus. Wine's DirectInput reads real evdev devices, so
#                  nothing injected above the kernel reaches the game.
#                  See runner/vkbd.py.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        openbox \
        xdotool \
        python3-evdev \
    && rm -rf /var/lib/apt/lists/*

# winetricks lives in Debian's contrib component, not main, so fetch the script
# directly rather than enabling an extra component for one file. It is used
# once per prefix to install the MSVC redistributable -- see entrypoint.sh for
# why Wine's builtin msvcp140 is not sufficient.
RUN wget -q -O /usr/local/bin/winetricks \
        https://raw.githubusercontent.com/Winetricks/winetricks/master/src/winetricks \
    && chmod +x /usr/local/bin/winetricks

# Force software rendering. Xvfb exposes no DRI device, so Mesa would fall back
# to llvmpipe regardless; setting it explicitly makes the behaviour intentional
# rather than incidental, and keeps the image working when /dev/dri IS mounted
# for VAAPI encoding (we want the GPU used for encode, not for render -- the
# game's GL output has to land in a PBO we can read back cheaply).
ENV LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=llvmpipe \
    WINEDEBUG=-all \
    PULSE_SERVER=/nonexistent-sfe \
    PYTHONUNBUFFERED=1

# No PulseAudio socket and no /dev/snd inside the container, so audio is silent
# by construction here. The runner still sets the Wine audio overrides because
# it is also used directly on a workstation, where it is not.

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
