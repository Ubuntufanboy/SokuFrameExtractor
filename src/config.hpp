#pragma once
// =========================================================================
// SokuFrameExtractor — config.hpp
// =========================================================================

#include <cstdint>
#include <string>

namespace sfe {

// -------------------------------------------------------------------------
// Build-time constants
// -------------------------------------------------------------------------

constexpr int GAME_WIDTH        = 640;
constexpr int GAME_HEIGHT       = 480;
constexpr int BYTES_PER_PIXEL   = 4; // BGRA-32
constexpr int FRAME_BUFFER_SIZE = GAME_WIDTH * GAME_HEIGHT * BYTES_PER_PIXEL;

// Ring buffer capacity (frames pre-allocated in locked memory).
// 512 × ~1.17 MB = ~600 MB.  A 32-bit Wine process under 64-bit Linux
// normally has a 2–3 GB address space, so this is safe.
constexpr int RING_CAPACITY     = 512;

// How many PBOs to rotate through for async glReadPixels readback.
// 3 gives enough pipeline depth even when the GPU is two frames behind.
constexpr int PBO_COUNT         = 3;

// Menu FSM timings
constexpr int MENU_SETTLE_FRAMES    = 30;
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
enum class AutoState {
    IDLE,
    WAIT_TITLE,
    NAV_TO_REPLAY_MENU,
    WAIT_REPLAY_LIST,
    SELECT_REPLAY,
    WAIT_BATTLE_START,
    EXTRACTING,
    WAIT_BATTLE_END,
    CYCLE_NEXT,
    ALL_DONE,
};

// -------------------------------------------------------------------------
// One captured frame passed through the ring buffer
// -------------------------------------------------------------------------
struct CapturedFrame {
    int      replay_index;
    int      frame_index;
    uint16_t p1_input;
    uint16_t p2_input;
    // pixels are stored directly in the ring slot — no separate heap alloc
};

// -------------------------------------------------------------------------
// Runtime configuration  (loaded from .ini)
// -------------------------------------------------------------------------
struct Config {
    // Paths
    std::string output_dir   = "soku_extract";
    std::string replay_dir   = "replay";

    // Video output
    // fifo_path: Windows path seen by Wine.  Wine maps Z:\ → Linux root,
    // so "Z:\\tmp\\sfe_video.fifo" opens /tmp/sfe_video.fifo on the host.
    std::string fifo_path    = "Z:\\tmp\\sfe_video.fifo";

    // Encoding
    bool        fast_forward = true;   // Remove frame limiter
    bool        verbose      = false;
    int         skip_frames  = 0;      // Skip first N frames per replay

    // Passed through to start_sfe.sh for informational purposes only;
    // actual encoding is controlled by the shell wrapper.
    bool        use_vaapi    = true;
};

} // namespace sfe