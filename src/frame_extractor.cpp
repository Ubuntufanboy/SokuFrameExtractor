// =========================================================================
// SokuFrameExtractor — frame_extractor.cpp
// =========================================================================
// FSM, BattleManager vtable hooks, input staging for OGLHook.
//
// Changes from original
// ---------------------
//   • Full FSM navigation implemented (WAIT_TITLE → NAV → SELECT →
//     WAIT_BATTLE_START → EXTRACTING → WAIT_BATTLE_END → CYCLE_NEXT).
//   • Key injection via SendInput (works with DirectInput / Wine).
//   • VideoEncoder::start() is called once in WAIT_TITLE so the encoder
//     thread can open the FIFO while the menu is still being navigated —
//     by the time WAIT_BATTLE_START is reached the output is ready.
//   • m_capturing is only set true after isFifoReady() confirms the
//     encoder thread has a destination, preventing ring buffer overflow.
//   • VirtualProtect flag changed PAGE_WRITECOPY → PAGE_READWRITE (Bug 5).
// =========================================================================

#include <winsock2.h>
#include <windows.h>

#include "frame_extractor.hpp"
#include "ogl_hook.hpp"
#include "logger.hpp"

#include <SokuLib.hpp>
#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>

namespace fs = std::filesystem;

namespace sfe {

// =========================================================================
// Game memory addresses (pinned to Soku v1.10a)
// =========================================================================
constexpr DWORD ADDR_BATTLE_MANAGER = 0x008985E4;
constexpr DWORD VTBL_CBATTLEMANAGER = 0x008588EC;
constexpr DWORD ADDR_FRAME_DELAY    = 0x008A0FF8;

constexpr int BM_PLAYER1_OFFSET = 0x0C;
constexpr int BM_PLAYER2_OFFSET = 0x10;
constexpr int CHAR_INPUT_OFFSET = 0x754;

constexpr int VTBL_DESTRUCT_IDX = 0;
constexpr int VTBL_PROCESS_IDX  = 3;

// =========================================================================
// Navigation sequences
// =========================================================================
// Each NavStep: wait `wait` game frames then send virtual key `vk`.
// The tickNav() helper advances one step per call (called once per frame).
//
// Key codes (default Soku config):
//   Z = 0x5A  — Confirm / attack-A
//   VK_UP    = 0x26, VK_DOWN = 0x28  — menu navigation
// =========================================================================

// Full initial navigation: title screen → main menu → Watch Replay →
// replay list → navigate to first replay → start it.
//
// Timing breakdown (@60 fps):
//   600 frames = 10 s  (game fully loaded at title)
//   Each UP press: 10 frames apart (≈167 ms – menu scrolls at 1 item/frame)
//   30 frames between Z presses for screen transitions
static const NavStep k_initial_nav[] = {
    { 600, 'Z'     },   // 10 s wait, then Z → enter main menu
    {  60, VK_UP   },   // give menu open animation 1 s to settle, then up #1
    {  10, VK_UP   },   // up #2
    {  10, VK_UP   },   // up #3
    {  10, VK_UP   },   // up #4
    {  10, VK_UP   },   // up #5
    {  10, VK_UP   },   // up #6  (cursor now on Watch Replay)
    {  30, 'Z'     },   // Z → enter Watch Replay
    {  30, 'Z'     },   // Z → enter/confirm replay list
    {  30, VK_DOWN },   // settle, then down #1
    {  10, VK_DOWN },   // down #2  (cursor at list position 2, 0-indexed)
    {  20, 'Z'     },   // Z → select replay file
    {  20, 'Z'     },   // Z → start playback
};

// Per-cycle navigation: after a replay ends the game returns to the list.
// One down moves the cursor to the next entry; Z starts it.
static const NavStep k_next_replay[] = {
    {  60, VK_DOWN },   // wait for result screen to settle, advance one entry
    {  20, 'Z'     },   // start next replay
};

// Helper: number of elements in a stack array
template<typename T, int N>
static constexpr int countof(const T (&)[N]) { return N; }

// =========================================================================
// Key injection
// =========================================================================

// Send a key-down + key-up pair via SendInput.
// SendInput updates the global key state, which DirectInput polls via
// GetAsyncKeyState / the Win32 HID layer — works for Wine/Soku.
static void pressKey(BYTE vk) {
    INPUT inputs[2] = {};
    inputs[0].type        = INPUT_KEYBOARD;
    inputs[0].ki.wVk      = static_cast<WORD>(vk);
    inputs[0].ki.dwFlags  = 0;               // key down

    inputs[1].type        = INPUT_KEYBOARD;
    inputs[1].ki.wVk      = static_cast<WORD>(vk);
    inputs[1].ki.dwFlags  = KEYEVENTF_KEYUP; // key up

    SendInput(2, inputs, sizeof(INPUT));
}

// =========================================================================
// Global singleton + hook function pointers
// =========================================================================
static FrameExtractor g_extractor;
FrameExtractor& getExtractor() { return g_extractor; }

static int   (__fastcall *s_origBattleProcess )(void* This, int edx)          = nullptr;
static void* (__fastcall *s_origBattleDestruct)(void* This, int edx, int dyn) = nullptr;

// =========================================================================
// Utility
// =========================================================================
static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// =========================================================================
// BattleManager vtable hooks
// =========================================================================
static int __fastcall HookedBattleProcess(void* This, int edx) {
    return s_origBattleProcess(This, edx);
}

static void* __fastcall HookedBattleDestruct(void* This, int edx, int dyn) {
    sfe::log("HookedBattleDestruct fired");
    g_extractor.onBattleEnd();
    return s_origBattleDestruct(This, edx, dyn);
}

// =========================================================================
// FrameExtractor
// =========================================================================
FrameExtractor::FrameExtractor() = default;
FrameExtractor::~FrameExtractor() { shutdown(); }

// -------------------------------------------------------------------------
// init
// -------------------------------------------------------------------------
bool FrameExtractor::init(const Config& cfg) {
    m_cfg           = cfg;
    m_last_log_time = std::chrono::steady_clock::now();

    constexpr float ring_mb = static_cast<float>(RING_CAPACITY)
                              * sizeof(FrameSlot) / (1024.0f * 1024.0f);

    sfe::log("=====================================================");
    sfe::log("FrameExtractor::init (OGL / VAAPI encode path)");
    sfe::log("  Output dir   : %s", m_cfg.output_dir.c_str());
    sfe::log("  FIFO path    : %s", m_cfg.fifo_path.c_str());
    sfe::log("  Ring buffer  : %d frames (%.1f MB, ~%.0f s @ 60 fps)",
             RING_CAPACITY, ring_mb, RING_CAPACITY / 60.0f);
    sfe::log("  Fast forward : %s", m_cfg.fast_forward ? "yes" : "no");
    sfe::log("=====================================================");

    // Create output directory
    try {
        fs::create_directories(fs::path(utf8_to_wide(m_cfg.output_dir)));
    } catch (const fs::filesystem_error& e) {
        sfe::log("ERROR: Cannot create output dir: %s", e.what());
        return false;
    }

    // Scan replay directory (for count / logging)
    scanReplayDirectory();
    if (m_replay_files.empty())
        sfe::log("WARNING: No .rep files found in replay dir");
    else
        sfe::log("Found %d replay file(s) — will stop after all are watched",
                 (int)m_replay_files.size());

    // Create VideoEncoder (start() called lazily from FSM)
    m_video_writer = std::make_unique<VideoEncoder>();
    sfe::log("VideoEncoder object created (start() called at first replay)");

    // Install BattleManager vtable hooks
    // FIX (Bug 5): Use PAGE_READWRITE, not PAGE_WRITECOPY.
    // PAGE_WRITECOPY is only valid for file-mapped regions; on Wine it can
    // silently fail for .rdata sections, leaving the vtable unpatched.
    {
        DWORD* vtbl    = reinterpret_cast<DWORD*>(VTBL_CBATTLEMANAGER);
        DWORD  oldProt = 0;
        VirtualProtect(vtbl, 16 * sizeof(DWORD), PAGE_READWRITE, &oldProt);

        s_origBattleProcess  = reinterpret_cast<decltype(s_origBattleProcess)>(
                                   vtbl[VTBL_PROCESS_IDX]);
        vtbl[VTBL_PROCESS_IDX]  = reinterpret_cast<DWORD>(HookedBattleProcess);

        s_origBattleDestruct = reinterpret_cast<decltype(s_origBattleDestruct)>(
                                   vtbl[VTBL_DESTRUCT_IDX]);
        vtbl[VTBL_DESTRUCT_IDX] = reinterpret_cast<DWORD>(HookedBattleDestruct);

        DWORD tmp = 0;
        VirtualProtect(vtbl, 16 * sizeof(DWORD), oldProt, &tmp);
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

        m_hooks_installed = true;
        sfe::log("BattleManager vtable hooks installed (PAGE_READWRITE)");
    }

    // Install OGL wglSwapBuffers hook
    if (!getOGLHook().install(m_video_writer.get(), &m_capturing)) {
        sfe::log("ERROR: OGLHook::install() failed — is opengl32.dll loaded?");
        return false;
    }
    sfe::log("OGL hook installed");

    // Start navigation FSM
    m_nav_step = m_nav_sub_timer = 0;
    transitionTo(AutoState::WAIT_TITLE);
    sfe::log("FrameExtractor::init complete — beginning automated navigation");
    return true;
}

// -------------------------------------------------------------------------
// shutdown
// -------------------------------------------------------------------------
void FrameExtractor::shutdown() {
    if (!m_hooks_installed && m_state == AutoState::IDLE) return;

    sfe::log("FrameExtractor::shutdown begin");

    m_capturing = false;
    getOGLHook().uninstall();

    if (m_limiter_removed) restoreFrameLimiter();

    if (m_video_writer) {
        m_video_writer->stop();
        m_video_writer.reset();
    }

    m_state           = AutoState::IDLE;
    m_hooks_installed = false;
    sfe::log("FrameExtractor::shutdown complete");
}

// -------------------------------------------------------------------------
// onHeartbeat — called from OGLHook::onBeforeSwap every render frame
// -------------------------------------------------------------------------
void FrameExtractor::onHeartbeat() {
    m_state_timer++;
    updateFSM();
}

// -------------------------------------------------------------------------
// onBattleEnd — called from BattleManager destructor hook (game thread)
// -------------------------------------------------------------------------
void FrameExtractor::onBattleEnd() {
    if (m_state == AutoState::EXTRACTING) {
        sfe::log("onBattleEnd: battle ended at frame %d", m_frame_index);
        transitionTo(AutoState::WAIT_BATTLE_END);
    }
}

// -------------------------------------------------------------------------
// bufferFillPercent
// -------------------------------------------------------------------------
int FrameExtractor::bufferFillPercent() const {
    if (!m_video_writer) return 0;
    return m_video_writer->ring().pendingCount() * 100 / RING_CAPACITY;
}

// =========================================================================
// Navigation helper
// =========================================================================
bool FrameExtractor::tickNav(const NavStep* seq, int count) {
    if (m_nav_step >= count) return true;

    m_nav_sub_timer++;
    const NavStep& s = seq[m_nav_step];

    if (m_nav_sub_timer >= s.wait) {
        if (s.vk != 0) {
            pressKey(s.vk);
            if (m_cfg.verbose)
                sfe::log("Nav: pressed VK 0x%02X (step %d/%d)", s.vk, m_nav_step + 1, count);
        }
        m_nav_step++;
        m_nav_sub_timer = 0;
    }

    return (m_nav_step >= count);
}

// =========================================================================
// FSM
// =========================================================================
void FrameExtractor::updateFSM() {
    switch (m_state) {

    // ------------------------------------------------------------------
    case AutoState::IDLE:
        break;

    // ------------------------------------------------------------------
    // WAIT_TITLE
    // Execute the full initial navigation sequence: press Z at title,
    // navigate up 6 times, enter Watch Replay, navigate replay list,
    // select and start the first replay.
    //
    // The VideoEncoder is started here (first call only) so the encoder
    // thread can open the FIFO in the background while menus are loading.
    // ------------------------------------------------------------------
    case AutoState::WAIT_TITLE:
        // Kick off encoder early so it can connect to the FIFO while
        // we are still navigating menus (~2 s of nav ahead of us).
        if (m_state_timer == 1) {
            if (!m_video_writer->start(m_cfg)) {
                sfe::log("ERROR: VideoEncoder::start() failed — aborting");
                transitionTo(AutoState::IDLE);
                break;
            }
            sfe::log("WAIT_TITLE: VideoEncoder started; encoder thread opening FIFO");
        }

        if (tickNav(k_initial_nav, countof(k_initial_nav))) {
            sfe::log("Initial navigation sequence complete — waiting for battle to start");
            transitionTo(AutoState::WAIT_BATTLE_START);
        }
        break;

    // ------------------------------------------------------------------
    // WAIT_BATTLE_START
    // Wait for the game to enter a battle scene.
    // Also wait for isFifoReady() so the encoder thread has an open
    // output destination before we start pumping frames into the ring.
    // A 60-second timeout retries the navigation if something went wrong.
    // ------------------------------------------------------------------
    case AutoState::WAIT_BATTLE_START:
        if (SokuLib::sceneId == SokuLib::SCENE_BATTLE) {
            // Gate on FIFO ready: ring buffer can hold ~8.5 s of frames
            // so a brief wait here is fine.
            if (!m_video_writer->isFifoReady()) {
                if (m_state_timer % 60 == 0)
                    sfe::log("WAIT_BATTLE_START: battle detected but FIFO not ready yet...");
                break;
            }

            char buf[64];
            snprintf(buf, sizeof(buf), "replay_%03d", m_replays_done + 1);
            m_current_replay_name = buf;
            m_current_replay_idx  = m_replays_done;
            m_frame_index         = 0;

            if (!m_video_writer->openReplay(m_current_replay_name))
                sfe::log("WARNING: openReplay() failed — CSV will be missing");

            if (m_cfg.fast_forward && !m_limiter_removed)
                removeFrameLimiter();

            m_capturing = true;
            transitionTo(AutoState::EXTRACTING);
        } else if (m_state_timer > 3600) {
            // 60-second timeout: navigation may have been disrupted.
            // Reset nav state and try the full sequence again.
            sfe::log("WAIT_BATTLE_START: 60s timeout — retrying navigation");
            m_nav_step = m_nav_sub_timer = 0;
            transitionTo(AutoState::WAIT_TITLE);
        }
        break;

    // ------------------------------------------------------------------
    // EXTRACTING
    // Stage inputs + frame index for OGLHook, advance per-replay counter,
    // and do periodic performance logging.
    // Exits when the battle scene ends or the frame limit is hit.
    // ------------------------------------------------------------------
    case AutoState::EXTRACTING:
        if (m_frame_index >= MAX_FRAMES_PER_REPLAY) {
            sfe::log("Max frame limit hit (%d)", MAX_FRAMES_PER_REPLAY);
            transitionTo(AutoState::WAIT_BATTLE_END);
            break;
        }
        if (SokuLib::sceneId != SokuLib::SCENE_BATTLE) {
            sfe::log("Scene changed — battle ended at frame %d", m_frame_index);
            transitionTo(AutoState::WAIT_BATTLE_END);
            break;
        }

        // Stage inputs for the OGL hook to bundle with the pixel data.
        {
            void*    bm = *reinterpret_cast<void**>(ADDR_BATTLE_MANAGER);
            uint16_t p1 = 0, p2 = 0;
            if (bm) {
                void* p1obj = *reinterpret_cast<void**>(
                                  reinterpret_cast<char*>(bm) + BM_PLAYER1_OFFSET);
                void* p2obj = *reinterpret_cast<void**>(
                                  reinterpret_cast<char*>(bm) + BM_PLAYER2_OFFSET);
                p1 = readPlayerInput(p1obj);
                p2 = readPlayerInput(p2obj);
            }
            OGLHook& hook          = getOGLHook();
            hook.staged_p1          = p1;
            hook.staged_p2          = p2;
            hook.staged_frame_index = m_frame_index;
        }

        m_frames_since_log++;
        {
            auto now     = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               now - m_last_log_time).count();
            if (elapsed >= 5) {
                float fps = m_frames_since_log / static_cast<float>(elapsed);
                sfe::log("Perf: %.1f fps staged, buffer %d%%, encoder wrote %d frames",
                         fps, bufferFillPercent(),
                         m_video_writer ? m_video_writer->totalFramesWritten() : 0);
                m_frames_since_log = 0;
                m_last_log_time    = now;
            }
        }

        m_frame_index++;
        break;

    // ------------------------------------------------------------------
    // WAIT_BATTLE_END
    // Brief settling period to let the game render its result screen,
    // then disarm capture, flush CSV, and move to CYCLE_NEXT.
    // ------------------------------------------------------------------
    case AutoState::WAIT_BATTLE_END:
        if (m_state_timer > 60) {
            m_capturing = false;    // disarm before closing replay

            if (m_limiter_removed) restoreFrameLimiter();

            m_video_writer->closeReplay();
            m_replays_done++;

            sfe::log("Replay done: %s (%d frames staged, %d written to encoder)",
                     m_current_replay_name.c_str(),
                     m_frame_index,
                     m_video_writer ? m_video_writer->totalFramesWritten() : 0);

            // Stop once we have watched all known replays.
            if (!m_replay_files.empty() &&
                m_replays_done >= static_cast<int>(m_replay_files.size())) {
                transitionTo(AutoState::ALL_DONE);
            } else {
                m_nav_step = m_nav_sub_timer = 0;
                transitionTo(AutoState::CYCLE_NEXT);
            }
        }
        break;

    // ------------------------------------------------------------------
    // CYCLE_NEXT
    // Send the "advance + start next replay" key sequence, then wait for
    // the battle scene.
    // ------------------------------------------------------------------
    case AutoState::CYCLE_NEXT:
        if (tickNav(k_next_replay, countof(k_next_replay))) {
            sfe::log("CYCLE_NEXT: nav keys sent, waiting for next battle");
            transitionTo(AutoState::WAIT_BATTLE_START);
        }
        break;

    // ------------------------------------------------------------------
    // ALL_DONE
    // All replays processed.  Stop the encoder (flushes FIFO + CSV),
    // then go idle.
    // ------------------------------------------------------------------
    case AutoState::ALL_DONE:
        sfe::log("ALL_DONE: %d replay(s) captured. Stopping encoder.", m_replays_done);
        if (m_video_writer) m_video_writer->stop();
        transitionTo(AutoState::IDLE);
        break;

    default:
        break;
    }
}

void FrameExtractor::transitionTo(AutoState next) {
    static const char* names[] = {
        "IDLE", "WAIT_TITLE", "NAV_TO_REPLAY_MENU", "WAIT_REPLAY_LIST",
        "SELECT_REPLAY", "WAIT_BATTLE_START", "EXTRACTING", "WAIT_BATTLE_END",
        "CYCLE_NEXT", "ALL_DONE"
    };
    sfe::log("FSM: %s -> %s",
             names[static_cast<int>(m_state)],
             names[static_cast<int>(next)]);
    m_state       = next;
    m_state_timer = 0;
}

// =========================================================================
// Input reading
// =========================================================================
uint16_t FrameExtractor::readPlayerInput(void* char_obj) {
    if (!char_obj) return 0;

    struct SWRCHARINPUT { int lr, ud, a, b, c, d, ch, s; };
    auto* inp = reinterpret_cast<SWRCHARINPUT*>(
                    reinterpret_cast<char*>(char_obj) + CHAR_INPUT_OFFSET);

    uint16_t mask = 0;
    if (inp->ud < 0) mask |= INPUT_UP;
    if (inp->ud > 0) mask |= INPUT_DOWN;
    if (inp->lr < 0) mask |= INPUT_LEFT;
    if (inp->lr > 0) mask |= INPUT_RIGHT;
    if (inp->a  > 0) mask |= INPUT_A;
    if (inp->b  > 0) mask |= INPUT_B;
    if (inp->c  > 0) mask |= INPUT_C;
    if (inp->d  > 0) mask |= INPUT_D;
    if (inp->ch > 0) mask |= INPUT_CHANGE;
    if (inp->s  > 0) mask |= INPUT_SPELL;
    return mask;
}

// =========================================================================
// Replay directory scan
// =========================================================================
void FrameExtractor::scanReplayDirectory() {
    m_replay_files.clear();
    fs::path root(utf8_to_wide(m_cfg.replay_dir));

    try {
        std::error_code ec;
        auto it  = fs::recursive_directory_iterator(
                       root, fs::directory_options::skip_permission_denied, ec);
        auto end = fs::recursive_directory_iterator();

        while (it != end && !ec) {
            const auto& entry = *it;
            if (entry.is_regular_file()) {
                std::wstring ext = entry.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](wchar_t c){ return std::towlower(c); });
                if (ext == L".rep")
                    m_replay_files.push_back(entry.path().wstring());
            }
            it.increment(ec);
        }
    } catch (...) {}

    std::sort(m_replay_files.begin(), m_replay_files.end());
}

// =========================================================================
// Frame limiter patch
// =========================================================================
void FrameExtractor::removeFrameLimiter() {
    auto* ptr = reinterpret_cast<int*>(ADDR_FRAME_DELAY);
    memcpy(m_original_delay_bytes, ptr, sizeof(int));
    m_sleep_patch_addr = ptr;

    DWORD oldProt = 0, tmp = 0;
    VirtualProtect(ptr, sizeof(int), PAGE_READWRITE, &oldProt);
    *ptr = 0;
    VirtualProtect(ptr, sizeof(int), oldProt, &tmp);

    m_limiter_removed = true;
    sfe::log("Frame limiter removed");
}

void FrameExtractor::restoreFrameLimiter() {
    if (!m_sleep_patch_addr) return;
    auto* ptr = reinterpret_cast<int*>(m_sleep_patch_addr);

    DWORD oldProt = 0, tmp = 0;
    VirtualProtect(ptr, sizeof(int), PAGE_READWRITE, &oldProt);
    memcpy(ptr, m_original_delay_bytes, sizeof(int));
    VirtualProtect(ptr, sizeof(int), oldProt, &tmp);

    m_limiter_removed  = false;
    m_sleep_patch_addr = nullptr;
    sfe::log("Frame limiter restored");
}

} // namespace sfe