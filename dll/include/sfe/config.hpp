#pragma once
// =========================================================================
// SokuFrameExtractor — config.hpp
// =========================================================================

// -------------------------------------------------------------------------
// NO C++ STANDARD LIBRARY THAT NEEDS MSVCP140
// -------------------------------------------------------------------------
// This module is injected into a game running under Wine. Wine's *builtin*
// msvcp140.dll does not implement everything the MSVC STL calls -- notably
// ?_Throw_Cpp_error@std@@YAXH@Z, which std::mutex and std::thread reach on
// their failure paths. Calling it aborts the process during module load, and
// the only trace is a zero-byte log: initLog()'s fopen succeeded, but nothing
// got as far as writing a line.
//
// Depending on the native redistributable being present would make every
// collection worker need it installed. Instead the module uses Win32
// primitives (CRITICAL_SECTION, CONDITION_VARIABLE, CreateThread) and the
// UCRT (stdio), which Wine implements completely. Header-only pieces of the
// standard library that emit no msvcp140 imports -- <atomic>, <memory>,
// <cstdint> -- are still fine.
//
// dll/CMakeLists.txt enforces this after every link: if MSVCP140 reappears in
// the import table, the build fails.
#include <cstdint>

namespace sfe {

// Fixed-size path buffer. Replaces std::string in Config: iostreams and the
// std::string throw helpers are exactly what dragged msvcp140 in.
constexpr int SFE_PATH_MAX = 520;   // MAX_PATH (260) with generous headroom

// -------------------------------------------------------------------------
// Build-time constants
// -------------------------------------------------------------------------

constexpr int GAME_WIDTH        = 640;
constexpr int GAME_HEIGHT       = 480;
constexpr int BYTES_PER_PIXEL   = 4; // BGRA-32
constexpr int FRAME_BUFFER_SIZE = GAME_WIDTH * GAME_HEIGHT * BYTES_PER_PIXEL;

// Ring buffer capacity, in frames.  Must be a power of two.
//
// This was 512 (~600 MB).  Two reasons it is now 128 (~150 MB, ~2 s @ 60 fps):
//   * The game is a 32-bit process with a 2-3 GB address space; a 600 MB
//     reservation is a large fraction of it for no measured benefit.
//   * Observed occupancy across a real capture run never exceeded 22%, and
//     VirtualLock on 600 MB failed outright (GLE=5) so the pages were not even
//     pinned.  128 slots still absorbs multi-second encoder stalls.
constexpr int RING_CAPACITY     = 128;

// How many PBOs to rotate through for async glReadPixels readback.
// 3 gives enough pipeline depth even when the GPU is two frames behind.
constexpr int PBO_COUNT         = 3;

// Safety limit: a Soku match is a few thousand frames.  This only exists so a
// wedged capture cannot fill the disk.
constexpr int MAX_FRAMES_PER_REPLAY = 300000; // ~83 min @ 60 fps

// -------------------------------------------------------------------------
// Input bitmasks (SokuLib / ReplayInputView+ convention)
// -------------------------------------------------------------------------
constexpr uint16_t INPUT_UP     = 0x001;
constexpr uint16_t INPUT_DOWN   = 0x002;
constexpr uint16_t INPUT_LEFT   = 0x004;
constexpr uint16_t INPUT_RIGHT  = 0x008;
constexpr uint16_t INPUT_A      = 0x010;
constexpr uint16_t INPUT_B      = 0x020;
constexpr uint16_t INPUT_C      = 0x040;
constexpr uint16_t INPUT_D      = 0x080;
constexpr uint16_t INPUT_CHANGE = 0x100;
constexpr uint16_t INPUT_SPELL  = 0x200;

constexpr uint16_t INPUT_DIR_MASK    = 0x00F;
constexpr uint16_t INPUT_BUTTON_MASK = 0x3F0;
constexpr uint16_t INPUT_ALL_MASK    = 0x3FF;

// -------------------------------------------------------------------------
// FSM states
// -------------------------------------------------------------------------
// One replay per process.  The runner stages exactly one .rep file into the
// replay directory before launching the game, so there is no list to walk, no
// cursor to track, and no cycling.
//
// The old design drove a 397-entry list inside one long-lived process and
// named outputs from a counter.  Two defects followed from that structure, and
// both are visible in the pre-refactor output under Soku/soku_extract/:
//
//   * Capture stayed armed while the FSM was between replays, so menu and
//     result-screen frames were recorded carrying a stale gameplay frame
//     index -- replay_003's CSV repeats frame 8622 31,789 times.
//   * The per-replay CSV was opened and closed on the game thread while the
//     encoder thread was still draining the previous replay's frames out of
//     the ring, so each file begins with the tail of its predecessor.  The
//     start index of each CSV is exactly the frame its predecessor was stuck
//     on: 9718 -> 8622 -> 7696, three links in a row.
//
// Neither is a patchable slip; both are consequences of one process owning
// many replays.  One replay per process removes the shared state entirely.
enum class AutoState {
    IDLE,
    WAIT_TITLE,         // wait for the game to finish loading, then advance
    ENTER_REPLAY_MENU,  // title -> main menu -> Watch Replay
    START_REPLAY,       // select the single staged entry and start it
    EXTRACTING,         // capturing frames
    DRAINING,           // battle over; let the encoder flush
    DONE,               // success: write status and exit
    FAILED,             // give up: write status and exit non-zero
};

const char* toString(AutoState s);

// -------------------------------------------------------------------------
// Runtime configuration  (loaded from SokuFrameExtractor.ini)
// -------------------------------------------------------------------------
// Every field here is read by the code.  The previous version parsed
// SkipFrames and UseVAAPI, logged them, and used neither, while the shipped
// .ini advertised three more keys (SaveAsBMP, EncoderThreads, UseRenderTarget)
// that no code had ever read.  Keep this struct and config/sfe.ini in sync --
// tests/test_config_parity.py enforces it.
struct Config {
    // Directory holding the single staged .rep file.
    char replay_dir[SFE_PATH_MAX]  = "replay";

    // Where per-replay output (inputs.csv) is written.
    char output_dir[SFE_PATH_MAX]  = "soku_extract";

    // Windows path to the video sink.  Wine maps Z:\ to the Linux root, so
    // "Z:\\tmp\\sfe_video.fifo" opens /tmp/sfe_video.fifo on the host.  The
    // runner creates the FIFO and attaches FFmpeg before launching the game.
    char fifo_path[SFE_PATH_MAX]   = "Z:\\tmp\\sfe_video.fifo";

    // Where to write the machine-readable run result the runner polls for.
    char status_path[SFE_PATH_MAX] = "Z:\\tmp\\sfe_status.json";

    // Remove the 60 fps limiter while capturing.
    bool fast_forward = true;

    bool verbose      = false;
};

} // namespace sfe
