#pragma once
// =========================================================================
// SokuFrameExtractor — session.hpp
// =========================================================================
// One capture session = one game process = one replay.
//
// The runner stages exactly one .rep file into the replay directory, launches
// the game, and waits.  This object drives the game to that replay, captures
// it, writes a status file, and exits the process.
//
// WHY ONE REPLAY PER PROCESS
// --------------------------
// The previous design walked a 397-entry replay list inside a single
// long-lived process.  Three problems followed from that shape, all of them
// visible in the shipped output under Soku/soku_extract/:
//
//   1. Identity was positional.  Output was named replay_%03d from a counter
//      and the .rep filename was never recorded, so a single navigation
//      hiccup silently relabelled every subsequent capture with no way to
//      detect it after the fact.
//   2. Capture stayed armed between replays, recording menus and result
//      screens under a frozen gameplay frame index (one file repeats frame
//      8622 31,789 times -- 79% of its rows).
//   3. The per-replay CSV was swapped on the game thread while the encoder
//      thread was still draining the previous replay, so each file starts
//      with its predecessor's tail.
//
// None of those are patchable in isolation; they are consequences of one
// process owning mutable state across many replays.  With one replay per
// process there is no counter, no cursor, no swap, and no cross-replay state:
// the runner knows exactly which file it staged, and a crash costs one replay
// instead of a batch.
//
// STARTING THE REPLAY
// -------------------
// No menu navigation and no synthetic input at all.  The session hooks the
// current scene's onProcess() -- the one function the engine asks "which
// scene next?" -- and from inside it runs exactly what the replay menu's
// confirm handler runs: InputManager::readReplay(), then setBattleMode() with
// the mode the .rep header declares, then return SCENE_LOADING.
//
// Driving the menus was tried first and cannot work headlessly: Soku reads
// the keyboard through DirectInput, and Wine's dinput reads real evdev
// devices rather than the synthetic Win32 input queue, so SendInput,
// SendInput+SetFocus, PostMessage(WM_KEYDOWN), xdotool/XTEST and a uinput
// virtual keyboard were all swallowed -- the game rendered its title screen
// while the scene id never moved.
//
// Calling the game directly is better than a working keypress would have
// been: there is no key timing to calibrate, no menu layout to assume, and a
// bad .rep is reported by the game itself instead of looking like a
// navigation failure.  Reaching the battle is still verified against the
// scene id, and failure to do so ends the process cleanly.
// =========================================================================

#include "sfe/config.hpp"
#include "sfe/ogl_hook.hpp"
#include "sfe/video_encoder.hpp"

#include <memory>   // unique_ptr: header-only, no msvcp140

namespace sfe {

class Session {
public:
    Session();
    ~Session();

    bool init(const Config& cfg);
    void shutdown();

    // Called once per presented frame from the OGL hook, on the game thread,
    // before the pixels are read.  Advances the FSM and returns the tag for
    // the frame being presented.
    FrameTag onFrame();

    // Called from the BattleManager destructor hook when the match ends.
    void onBattleEnd();

    AutoState state() const { return m_state; }

private:
    void transitionTo(AutoState next);
    void finish(AutoState terminal, const char* reason);

    // Writes the machine-readable result the runner polls for.
    void writeStatus(const char* status, const char* reason);

    // writeStatus() followed by ExitProcess().  Never returns.
    // MUST NOT be called from init() -- that runs under the loader lock.
    [[noreturn]] void writeStatusAndExit(const char* status, const char* reason);

    // Starts the encoder thread.  Deferred out of init() because spawning a
    // thread under the Windows loader lock crashes module load.
    bool ensureEncoderStarted();

    uint16_t readPlayerInput(void* char_obj) const;
    bool     findStagedReplay();

    void removeFrameLimiter();
    void restoreFrameLimiter();

    // Wall-clock milliseconds since the session started.
    uint32_t elapsedMs() const;

    Config m_cfg;
    std::unique_ptr<VideoEncoder> m_encoder;

    AutoState m_state = AutoState::IDLE;

    // The single .rep this process was launched to capture.  Recorded in the
    // status file so the runner can prove output matches input.
    char m_replay_name[SFE_PATH_MAX] = {};

    int      m_frame_index   = 0;   // engine ticks captured so far
    uint32_t m_start_tick    = 0;   // GetTickCount at session start
    uint32_t m_state_tick    = 0;   // GetTickCount at last transition
    uint32_t m_state_tick_logged = 0;  // throttles START_REPLAY scene logging
    int      m_last_scene_logged = -1; // so each scene change is logged once

    bool m_hooks_installed = false;
    bool m_vtable_hooked   = false;
    bool m_scene_hooked    = false;
    bool m_start_armed     = false;
    bool m_capturing       = false;
    bool m_encoder_started = false;

    // Frame-limiter patch state.
    uint8_t m_original_delay_bytes[4] = {};
    void*   m_sleep_patch_addr        = nullptr;
    bool    m_limiter_removed         = false;

    // Periodic throughput logging. GetTickCount rather than <chrono>'s
    // steady_clock, to keep this header free of anything that could reach
    // msvcp140 (see config.hpp).
    uint32_t m_last_log_tick   = 0;
    int      m_frames_since_log = 0;
};

Session& getSession();

} // namespace sfe
