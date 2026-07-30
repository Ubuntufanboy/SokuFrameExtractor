#pragma once
// =========================================================================
// SokuFrameExtractor — frame_extractor.hpp
// =========================================================================
// FSM + BattleManager hooks + automated replay navigation.
//
// Capture path (handled by OGLHook):
//   OGLHook intercepts wglSwapBuffers and writes BGRA frames directly into
//   VideoEncoder's RingBuffer via async PBO readback.  FrameExtractor only
//   manages the FSM, replay bookkeeping, input staging, and the frame
//   limiter patch.
//
// Navigation:
//   The FSM now sends synthesised keystrokes (SendInput) to navigate the
//   game's menus automatically: title → main menu → Watch Replay →
//   replay list → start replay → loop.
// =========================================================================

#include "config.hpp"
#include "video_encoder.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace sfe {

// -------------------------------------------------------------------------
// Navigation step: wait `wait` game frames, then press virtual key `vk`.
// vk == 0 means "just wait, no key press".
// -------------------------------------------------------------------------
struct NavStep {
    int   wait;   // frames to wait before pressing
    BYTE  vk;     // Windows virtual key code (0 = wait only)
};

// -------------------------------------------------------------------------
class FrameExtractor {
public:
    FrameExtractor();
    ~FrameExtractor();

    bool init(const Config& cfg);
    void shutdown();

    // Called from OGLHook::onBeforeSwap every frame (game thread).
    void onHeartbeat();

    // Called from BattleManager destructor vtable hook (game thread).
    void onBattleEnd();

    AutoState state()  const { return m_state; }
    int replaysDone()  const { return m_replays_done; }
    int replaysTotal() const { return static_cast<int>(m_replay_files.size()); }

    int framesCaptured() const {
        return m_video_writer ? m_video_writer->totalFramesWritten() : 0;
    }

    int bufferFillPercent() const;

private:
    // ------------------------------------------------------------------
    // FSM
    // ------------------------------------------------------------------
    void updateFSM();
    void transitionTo(AutoState next);

    // Navigation helper — advances through a NavStep sequence one step per
    // frame.  Returns true when the entire sequence has been executed.
    bool tickNav(const NavStep* seq, int count);

    // ------------------------------------------------------------------
    // Replay management
    // ------------------------------------------------------------------
    void scanReplayDirectory();

    // ------------------------------------------------------------------
    // Input reading
    // ------------------------------------------------------------------
    uint16_t readPlayerInput(void* char_obj);

    // ------------------------------------------------------------------
    // Frame limiter
    // ------------------------------------------------------------------
    void removeFrameLimiter();
    void restoreFrameLimiter();

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------
    Config m_cfg;

    std::unique_ptr<VideoEncoder> m_video_writer;
    bool m_capturing = false;   // pointer passed to OGLHook::install()

    AutoState m_state       = AutoState::IDLE;
    int       m_state_timer = 0;

    std::vector<std::wstring> m_replay_files;
    int         m_current_replay_idx  = -1;
    int         m_replays_done        = 0;
    std::string m_current_replay_name;

    int  m_frame_index     = 0;
    bool m_hooks_installed = false;

    // Frame-limiter patch
    uint8_t m_original_delay_bytes[4] = {};
    void*   m_sleep_patch_addr        = nullptr;
    bool    m_limiter_removed         = false;

    // Navigation state
    int     m_nav_step      = 0;   // index into the current NavStep array
    int     m_nav_sub_timer = 0;   // frames waited in the current step

    // Performance logging
    std::chrono::steady_clock::time_point m_last_log_time;
    int m_frames_since_log = 0;
};

FrameExtractor& getExtractor();

} // namespace sfe