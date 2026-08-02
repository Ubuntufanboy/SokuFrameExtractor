# Source this to put the msvc-wine toolchain on PATH for a host build.
#
#   source scripts/msvc-env.sh
#   cmake --preset msvc-wine && cmake --build --preset msvc-wine
#
# Only needed for building on the host. docker/build.Dockerfile sets the same
# variables itself, and neither is needed to *run* collection.
#
# MSVC_ROOT points at an msvc-wine installation (https://github.com/mstorsjo/msvc-wine).
# Override either variable before sourcing if yours lives elsewhere.
: "${MSVC_ROOT:=$HOME/my_msvc/opt/msvc}"
: "${MSVC_WINEPREFIX:=$HOME/.wine-msvc}"

if [ ! -x "$MSVC_ROOT/bin/x86/cl" ]; then
    echo "msvc-env: no cl at $MSVC_ROOT/bin/x86/cl" >&2
    echo "          set MSVC_ROOT, or use docker/build.Dockerfile instead." >&2
    return 1 2>/dev/null || exit 1
fi

export WINEPREFIX="$MSVC_WINEPREFIX"
export PATH="$MSVC_ROOT/bin/x86:$PATH"
export WINEDEBUG=-all

# Wine grabs an audio device for every compiler invocation otherwise, which on
# a workstation is both pointless and audible.
export WINEDLLOVERRIDES="winepulse=d"

wineserver -p 2>/dev/null || true

echo "msvc-wine ready"
echo "  cl:     $(command -v cl)"
echo "  prefix: $WINEPREFIX"
