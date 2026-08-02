# ---------------------------------------------------------------------------
# Reproducible build of SokuFrameExtractor.dll (32-bit Windows) from Linux.
# ---------------------------------------------------------------------------
#   docker build -f docker/build.Dockerfile -t sfe-build .
#   docker run --rm -v "$PWD:/src" -v "$PWD/build:/src/build" sfe-build
#
# LICENSING -- READ BEFORE PUSHING THIS IMAGE ANYWHERE
# ----------------------------------------------------
# msvc-wine's install script downloads the MSVC toolchain and Windows SDK from
# Microsoft under the Visual Studio licence. That licence does not permit
# redistribution, so this image is for local and CI use only and must never be
# pushed to a public registry. The download happens at build time on the
# machine doing the building, which is what keeps that acceptance with whoever
# runs it.
#
# WHY MSVC AND NOT MINGW
# ----------------------
# The module binds the game's C++ objects through hardcoded vtable indices and
# __thiscall/__fastcall member pointers -- MSVC ABI details. A MinGW build
# links cleanly and then corrupts the game at runtime, which is a far worse
# failure than a build error, so CMakeLists.txt refuses to configure with
# anything else.
FROM debian:bookworm-slim

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        ninja-build \
        git \
        python3 \
        msitools \
        winbind \
        wine \
        wine64 \
        binutils-mingw-w64-i686 \
        curl \
    && rm -rf /var/lib/apt/lists/*

# binutils-mingw-w64-i686 is not used to compile anything -- it provides
# i686-w64-mingw32-objdump, which the post-link guard in dll/CMakeLists.txt
# uses to assert the DLL does not import MSVCP140. Without objdump present
# that check silently downgrades to a warning.

ARG MSVC_WINE_REF=master
RUN git clone --depth 1 --branch "${MSVC_WINE_REF}" \
        https://github.com/mstorsjo/msvc-wine.git /opt/msvc-wine

# Downloads the MSVC toolchain from Microsoft (see the licence note above).
RUN cd /opt/msvc-wine && \
    python3 ./vsdownload.py --accept-license --dest /opt/msvc && \
    ./install.sh /opt/msvc

ENV PATH="/opt/msvc/bin/x86:${PATH}" \
    WINEPREFIX=/opt/wine-msvc \
    WINEDEBUG=-all \
    # Wine will otherwise grab an audio device during compiler invocations.
    WINEDLLOVERRIDES="winepulse=d"

WORKDIR /src

# Fetches SokuLib at its pinned commit and applies the vendored patch, then
# configures and builds. Kept as a command rather than a build step so the
# image stays reusable across source changes.
CMD ["/bin/bash", "-lc", "\
    ./scripts/fetch-deps.sh && \
    cmake --preset msvc-wine && \
    cmake --build --preset msvc-wine && \
    ls -la build/msvc-wine/dll/SokuFrameExtractor.dll"]
