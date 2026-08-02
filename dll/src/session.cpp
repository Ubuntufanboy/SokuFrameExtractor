// =========================================================================
// SokuFrameExtractor — session.cpp
// =========================================================================
// See session.hpp for why this is one-replay-per-process.
// =========================================================================

#include <winsock2.h>
#include <windows.h>

#include "sfe/session.hpp"
#include "sfe/logger.hpp"

#include <shlwapi.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace sfe {

// -------------------------------------------------------------------------
// Scene id, read straight from game memory.
// -------------------------------------------------------------------------
// This is all we ever used SokuLib for:
//
//     SokuLib::Scene &sceneId = *reinterpret_cast<Scene*>(ADDR_SCENE_ID);
//
// Including <SokuLib.hpp> for one dereference pulled the whole library in,
// and parts of it (Packet.cpp's `stream <<`) drag in iostreams and therefore
// msvcp140 -- the dependency this module exists to avoid. Reading the address
// directly costs nothing and removes the link dependency entirely.
constexpr DWORD ADDR_SCENE_ID = 0x008A0044;   // SokuLib::ADDR_SCENE_ID

enum SceneId : int {
    SCENE_LOGO = 0, SCENE_OPENING, SCENE_TITLE, SCENE_SELECT,
    SCENE_BATTLE = 5, SCENE_LOADING,
    SCENE_BATTLEWATCH = 15,
};

static inline int currentScene() {
    return *reinterpret_cast<volatile int*>(ADDR_SCENE_ID);
}

// Create a directory and any missing parents, Win32 only.
static bool makeDirs(const char* path) {
    char buf[SFE_PATH_MAX];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (char* p = buf; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            const char saved = *p;
            *p = '\0';
            if (*buf && !(p == buf + 2 && buf[1] == ':'))
                CreateDirectoryA(buf, nullptr);
            *p = saved;
        }
    }
    CreateDirectoryA(buf, nullptr);
    const DWORD a = GetFileAttributesA(buf);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// =========================================================================
// Game memory addresses (Soku v1.10a)
// =========================================================================
// These are absolute addresses into the game image, so they are only valid
// for the exact build main.cpp's CheckVersion() accepts.  A mismatched build
// does not fail gracefully here -- it writes function pointers into whatever
// happens to live at 0x008588EC.  That is why CheckVersion is a real check
// now instead of an unconditional `return true`.
constexpr DWORD ADDR_BATTLE_MANAGER = 0x008985E4;
constexpr DWORD VTBL_CBATTLEMANAGER = 0x008588EC;
constexpr DWORD ADDR_FRAME_DELAY    = 0x008A0FF8;

constexpr int BM_PLAYER1_OFFSET = 0x0C;
constexpr int BM_PLAYER2_OFFSET = 0x10;
constexpr int CHAR_INPUT_OFFSET = 0x754;

constexpr int VTBL_DESTRUCT_IDX = 0;

// =========================================================================
// Timing budgets (wall clock, milliseconds)
// =========================================================================
// Deliberately wall-clock rather than frame counts.  The old NavStep list
// counted frames, which meant every timing silently changed meaning once the
// frame limiter was removed -- "wait 600 frames" is 10 s at 60 fps and under
// 1 s at the 700+ fps the game reaches unthrottled.  It only worked because
// the limiter happened to still be engaged during navigation.
// Upper bound on reaching the title screen. Not a delay -- WAIT_TITLE polls
// the scene id and moves on the moment the title appears; this only bounds how
// long we wait before proceeding anyway. Generous because a CPU-capped
// container running llvmpipe boots far slower than real hardware: at a fixed
// 10 s the game was still on the logo (scene 0).
constexpr uint32_t MS_TITLE_DEADLINE = 60000;
constexpr uint32_t MS_BATTLE_DEADLINE = 90000; // nav start -> battle, else fail
constexpr uint32_t MS_DRAIN          = 1500;   // post-match settle before stop
constexpr uint32_t MS_FIFO_DEADLINE  = 60000;  // wait for FFmpeg to attach

// =========================================================================
// Driving the menus by hooking the game's own key check
// =========================================================================
// This is how menu navigation actually happens. Every attempt to deliver a
// keystroke from outside failed, because Soku reads the keyboard through
// DirectInput and Wine's dinput does not see synthetic input:
//
//   SendInput / +focus / +window manager .... swallowed
//   PostMessage(WM_KEYDOWN)  ................ swallowed
//   xdotool (XTEST, X server level) ......... swallowed
//   /dev/uinput virtual keyboard ............ works at the kernel level, but
//       the device is global to the machine -- it types into the host's
//       desktop, and an unprivileged container has no /dev/uinput at all.
//
// So stop trying to synthesise input and intercept the question instead.
// checkKeyOneshot(dik, ...) is what the menus call to ask "was this key just
// pressed?". Returning true once is exactly a single press -- "oneshot" is
// already the semantics we want, so there is no key-repeat or hold duration
// to tune, and nothing below the game to fight.
//
// The hook is the same 5-byte JMP trampoline used for wglSwapBuffers, which
// is already proven to work in this process.
constexpr DWORD ADDR_CHECK_KEY_ONESHOT = 0x0043DE30;

// DirectInput scancodes for the keys Soku's menus use.
constexpr int DIK_RETURN = 0x1C;
constexpr int DIK_Z      = 0x2C;
constexpr int DIK_UP     = 0xC8;
constexpr int DIK_DOWN   = 0xD0;

using PFN_checkKeyOneshot = int (__cdecl*)(int, int, int, int);
static PFN_checkKeyOneshot s_origCheckKey = nullptr;

// Set to a DIK code to report that key as "just pressed" exactly once.
// volatile: written by the FSM on the game thread, read inside the hook.
static volatile int s_inject_dik = 0;

// Diagnostic: record the argument tuples the game actually passes, so the
// meaning of the parameters can be read off a real run instead of assumed.
// The first argument was taken to be a DirectInput scancode; that assumption
// is what this exists to check.
static volatile int  s_log_calls = 40;  // log the first 40 distinct tuples, whenever they occur
static int s_seen[32][4] = {};
static int s_seen_n = 0;

static int __cdecl HookedCheckKeyOneshot(int a, int b, int c, int d) {
    if (s_log_calls > 0) {
        bool dup = false;
        for (int i = 0; i < s_seen_n; ++i)
            if (s_seen[i][0]==a && s_seen[i][1]==b && s_seen[i][2]==c && s_seen[i][3]==d)
                { dup = true; break; }
        if (!dup && s_seen_n < 32) {
            s_seen[s_seen_n][0]=a; s_seen[s_seen_n][1]=b;
            s_seen[s_seen_n][2]=c; s_seen[s_seen_n][3]=d;
            ++s_seen_n;
            sfe::log("checkKey call: (%d, %d, %d, %d)  [0x%X]", a, b, c, d, a);
            --s_log_calls;
        }
    }
    if (s_inject_dik != 0 && a == s_inject_dik) {
        s_inject_dik = 0;   // consume: one press, one report
        return 1;
    }
    return s_origCheckKey ? s_origCheckKey(a, b, c, d) : 0;
}

// Patch a 5-byte relative JMP over `target`, returning a callable trampoline
// for the original. Same technique as OGLHook::install.
static void* installJmpHook(void* target, void* replacement, uint8_t saved[5]) {
    memcpy(saved, target, 5);

    uint8_t patch[5] = { 0xE9, 0, 0, 0, 0 };
    const intptr_t rel = reinterpret_cast<intptr_t>(replacement)
                       - reinterpret_cast<intptr_t>(target) - 5;
    memcpy(patch + 1, &rel, 4);

    DWORD oldProt = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProt)) return nullptr;
    memcpy(target, patch, 5);
    VirtualProtect(target, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    void* tramp = VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    auto* tb = static_cast<uint8_t*>(tramp);
    memcpy(tb, saved, 5);
    tb[5] = 0xE9;
    const intptr_t back = reinterpret_cast<intptr_t>(target) + 5
                        - reinterpret_cast<intptr_t>(tb + 5) - 5;
    memcpy(tb + 6, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);

    DWORD tmp = 0;
    VirtualProtect(tramp, 16, PAGE_EXECUTE_READ, &tmp);
    return tramp;
}

// =========================================================================
// Navigation script
// =========================================================================
// Empirical, and the one part of this file that cannot be derived from
// anything observable: while the main menu is open the scene id stays
// SCENE_TITLE, so there is no state to poll between these presses.
//
// Exactly one .rep is staged in the replay directory, so the replay list has a
// single entry and the cursor already sits on it -- no scrolling, nothing to
// drift.  The old script pressed DOWN twice to reach "list position 2" of a
// 397-entry list, which is precisely the fragility this design removes.
//
// What IS verified is the outcome: reaching the battle is confirmed against
// SokuLib::sceneId, and failure to do so within MS_BATTLE_DEADLINE ends the
// process cleanly instead of retrying into an unknown menu.
template<typename T, int N>
static constexpr int countof(const T (&)[N]) { return N; }

// -------------------------------------------------------------------------
// Menu navigation by synthesised keypress
// -------------------------------------------------------------------------
// Used because the programmatic start (loadReplayFile + changeScene) faults
// inside the game's own battle creation: forcing SCENE_LOADING skips setup
// that the replay menu performs, leaving the BattleManager global at
// 0x008985E4 null when a thunk at 0x4386a6 dereferences it. Verified not to be
// our vtable hook -- it crashes identically with the hook disabled.
//
// SendInput needs a focused window, which bare Xvfb never provides; the runner
// now starts a window manager for exactly this reason.
struct NavStep {
    uint32_t delay_ms;  // settle time before this step
    int      dik;       // DirectInput scancode to report as pressed
};

static const NavStep k_nav[] = {
    { 1200, DIK_Z    },  // title -> main menu
    {  800, DIK_UP   },  // wrap upward to "Watch Replay"
    {  250, DIK_UP   },
    {  250, DIK_UP   },
    {  250, DIK_UP   },
    {  250, DIK_UP   },
    {  250, DIK_UP   },
    {  600, DIK_Z    },  // enter Watch Replay
    {  900, DIK_Z    },  // confirm / open the replay list
    {  900, DIK_Z    },  // select the single staged entry
    {  600, DIK_Z    },  // start playback
};

// The game's main window; must not match the 1x1 IME/helper windows.
static HWND gameWindow() {
    static HWND s_hwnd = nullptr;
    if (s_hwnd && IsWindow(s_hwnd)) return s_hwnd;
    s_hwnd = nullptr;
    const DWORD self = GetCurrentProcessId();
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != self) continue;
        RECT r{};
        if (!GetClientRect(h, &r)) continue;
        if ((r.right - r.left) < GAME_WIDTH || (r.bottom - r.top) < GAME_HEIGHT)
            continue;
        s_hwnd = h;
        break;
    }
    return s_hwnd;
}

static void pressKey(BYTE vk) {
    if (HWND h = gameWindow()) {
        SetForegroundWindow(h);
        SetActiveWindow(h);
        SetFocus(h);
    }
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// =========================================================================
// Key injection
// =========================================================================
// SendInput updates the global key state that DirectInput polls.  External
// X11 injection (xdotool) does NOT reach the game -- verified under Xvfb --
// which is why this has to happen in-process.
// =========================================================================
// Starting a replay programmatically
// =========================================================================
// The game is driven directly instead of by synthesising keypresses.
//
// Every synthetic-input approach failed under Xvfb, because Soku reads the
// keyboard through DirectInput and Wine's dinput reads raw X11/evdev state
// rather than the synthetic Win32 queue. Tried and confirmed dead:
// SendInput; SendInput plus SetForegroundWindow/SetFocus; PostMessage of
// WM_KEYDOWN/WM_KEYUP straight to the window; and xdotool from outside the
// process. In every case the game rendered its title screen happily while the
// scene id sat at SCENE_TITLE until the deadline.
//
// The game exposes exactly what we need:
//
//   InputManager::readReplay(path)   loads a .rep into the input manager
//   changeScene(SCENE_LOADING)       sets newSceneId and kicks the loader
//
// which is how the replay menu starts playback once you have chosen a file.
// Calling them directly skips the menus altogether. That removes the entire
// navigation script, the timing guesses in it, and the whole class of "a
// keypress landed in the wrong menu" failure -- the replay either loads or it
// does not, and we find out immediately.
//
// All addresses are from SokuLib's SokuAddresses.hpp for v1.10a, which
// main.cpp's PE-header check has already confirmed.
constexpr DWORD ADDR_COMM_MODE              = 0x00898690;  // SokuLib mainMode
constexpr DWORD ADDR_SUB_MODE               = 0x00898688;  // SokuLib subMode
constexpr DWORD ADDR_INPUT_MANAGER          = 0x00898718;
constexpr DWORD ADDR_READ_REPLAY            = 0x0042EAC0;
constexpr DWORD ADDR_SCENE_ID_NEW           = 0x008A0040;
constexpr DWORD ADDR_LOAD_GRAPHICS_FUN      = 0x00408410;
constexpr DWORD ADDR_LOAD_GRAPHICS_THREAD   = 0x0089FFF4;
constexpr DWORD ADDR_LOAD_GRAPHICS_THREADID = 0x0089FFF8;

// InputManager::readReplay is __thiscall: `this` in ECX, path on the stack.
using PFN_readReplay = bool (__thiscall*)(void* self, const char* path);

// SokuLib::BattleMode / BattleSubMode values.
constexpr unsigned char BATTLE_MODE_VSPLAYER  = 3;
constexpr unsigned char BATTLE_SUBMODE_REPLAY = 2;

// Tell the game this is replay playback before loading anything.
//
// Without it, changeScene(SCENE_LOADING) starts the loading screen and the
// game then faults at 0x4386a6 -- inside its own battle-creation code, right
// next to ADDR_BATTLE_MANAGER_CREATER. The BattleManager has to know it is
// driving from a replay stream rather than from live players; left in its
// default mode it looks for player state a replay never sets up.
//
// The two mode globals are written directly rather than calling the game's
// setBattleMode(). Calling it from here raised C000000D
// (STATUS_INVALID_PARAMETER) -- the convention matches SokuLib's binding, but
// we are on the render thread inside the swap hook while the game sits in the
// title scene, which is not a context it expects to be called from. These are
// plain byte variables (SokuLib exposes them as mainMode/subMode), so writing
// them has no call-context requirements.
static void setReplayBattleMode() {
    *reinterpret_cast<volatile unsigned char*>(ADDR_COMM_MODE) = BATTLE_MODE_VSPLAYER;
    *reinterpret_cast<volatile unsigned char*>(ADDR_SUB_MODE)  = BATTLE_SUBMODE_REPLAY;
    sfe::log("Battle mode set: main=%d sub=%d (replay playback)",
             BATTLE_MODE_VSPLAYER, BATTLE_SUBMODE_REPLAY);
}

// Load `path` into the game's input manager. Returns the game's own verdict,
// so a corrupt or unreadable .rep is reported rather than silently producing
// an empty capture.
static bool loadReplayFile(const char* path) {
    auto readReplay = reinterpret_cast<PFN_readReplay>(ADDR_READ_REPLAY);
    void* mgr = reinterpret_cast<void*>(ADDR_INPUT_MANAGER);
    return readReplay(mgr, path);
}

// SokuLib::changeScene -- request a scene and start the loader thread that
// performs the transition.
static void changeScene(int scene) {
    auto* new_scene = reinterpret_cast<volatile int*>(ADDR_SCENE_ID_NEW);
    if (currentScene() == scene) return;

    *new_scene = scene;
    HANDLE h = CreateThread(
        nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(ADDR_LOAD_GRAPHICS_FUN),
        nullptr, 0,
        reinterpret_cast<LPDWORD>(ADDR_LOAD_GRAPHICS_THREADID));
    *reinterpret_cast<volatile HANDLE*>(ADDR_LOAD_GRAPHICS_THREAD) = h;
}

// =========================================================================
// Globals
// =========================================================================
static Session g_session;
Session& getSession() { return g_session; }

const char* toString(AutoState s) {
    switch (s) {
    case AutoState::IDLE:              return "IDLE";
    case AutoState::WAIT_TITLE:        return "WAIT_TITLE";
    case AutoState::ENTER_REPLAY_MENU: return "ENTER_REPLAY_MENU";
    case AutoState::START_REPLAY:      return "START_REPLAY";
    case AutoState::EXTRACTING:        return "EXTRACTING";
    case AutoState::DRAINING:          return "DRAINING";
    case AutoState::DONE:              return "DONE";
    case AutoState::FAILED:            return "FAILED";
    }
    return "?";
}

// Only the destructor is hooked.  The old code also hooked Process (vtable
// index 3) with a function that did nothing but call through -- the riskiest
// hook in the codebase, taken every logic tick, for no effect.
static void* (__fastcall *s_origBattleDestruct)(void* This, int edx, int dyn) = nullptr;

static void* __fastcall HookedBattleDestruct(void* This, int edx, int dyn) {
    g_session.onBattleEnd();
    return s_origBattleDestruct(This, edx, dyn);
}

// Trampoline: OGLHook calls this once per presented frame.
static FrameTag SessionTagFn(void* user) {
    return static_cast<Session*>(user)->onFrame();
}

// =========================================================================
// Utility
// =========================================================================
// Minimal JSON string escaping -- replay names come from user filenames.
static void json_escape(const char* s, char* out, int cap) {
    int o = 0;
    auto put = [&](const char* t) {
        while (*t && o < cap - 1) out[o++] = *t++;
    };
    for (const char* p = s; *p && o < cap - 1; ++p) {
        switch (*p) {
        case '"':  put("\\\""); break;
        case '\\': put("\\\\"); break;
        case '\n': put("\\n");  break;
        case '\r': put("\\r");  break;
        case '\t': put("\\t");  break;
        default:
            if (static_cast<unsigned char>(*p) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", *p);
                put(buf);
            } else {
                out[o++] = *p;
            }
        }
    }
    out[o] = '\0';
}

// =========================================================================
// Session
// =========================================================================
Session::Session()  = default;
Session::~Session() { shutdown(); }

uint32_t Session::elapsedMs() const {
    return GetTickCount() - m_start_tick;
}

// -------------------------------------------------------------------------
bool Session::init(const Config& cfg) {
    m_cfg           = cfg;
    m_start_tick    = GetTickCount();
    m_state_tick    = m_start_tick;
    m_last_log_tick = GetTickCount();

    constexpr float ring_mb =
        static_cast<float>(RING_CAPACITY) * sizeof(FrameSlot) / (1024.0f * 1024.0f);

    sfe::log("=====================================================");
    sfe::log("Session::init  (one replay per process)");
    sfe::log("  Replay dir : %s", m_cfg.replay_dir);
    sfe::log("  Output dir : %s", m_cfg.output_dir);
    sfe::log("  FIFO       : %s", m_cfg.fifo_path);
    sfe::log("  Status     : %s", m_cfg.status_path);
    sfe::log("  Ring       : %d frames (%.0f MB, ~%.1f s @ 60 fps)",
             RING_CAPACITY, ring_mb, RING_CAPACITY / 60.0f);
    sfe::log("  FastFwd    : %s", m_cfg.fast_forward ? "yes" : "no");
    sfe::log("=====================================================");

    // Failures below report through the status file and return false rather
    // than calling ExitProcess: we are under the loader lock here, and tearing
    // the process down from inside DllMain is what produces a "crashed while
    // loading" verdict from SWRSToys and gets the module auto-disabled.
    if (!findStagedReplay()) {
        writeStatus("failed", "no .rep file staged in replay directory");
        return false;
    }
    sfe::log("Staged replay: %s", m_replay_name);

    if (!makeDirs(m_cfg.output_dir)) {
        sfe::log("ERROR: cannot create output dir %s (GLE=%lu)",
                 m_cfg.output_dir, GetLastError());
        writeStatus("failed", "cannot create output directory");
        return false;
    }

    // Construct the encoder, but DO NOT start it here.
    //
    // Initialize() runs inside SWRSToys' DllMain, i.e. under the Windows
    // loader lock. VideoEncoder::start() spawns a std::thread, and creating a
    // thread under the loader lock is the classic way to deadlock or crash a
    // process during module load -- the new thread's own DLL_THREAD_ATTACH
    // notification needs the very lock we are holding.
    //
    // Getting this wrong is not a graceful failure: the game crashes while
    // loading the module, and SWRSToys then records the crash in
    // currentModule.txt and *auto-disables the module* on the next launch,
    // with a modal dialog that blocks startup. An unattended run just stops
    // producing data and every subsequent launch hangs on the dialog.
    //
    // The encoder is started from the first onFrame() instead, which runs on
    // the game thread with no lock held.
    m_encoder = std::make_unique<VideoEncoder>();

    // --- BattleManager destructor hook -----------------------------------
    if (getenv("SFE_NO_VTABLE_HOOK")) {
        sfe::log("SFE_NO_VTABLE_HOOK set — skipping BattleManager vtable patch");
    } else {
        DWORD* vtbl    = reinterpret_cast<DWORD*>(VTBL_CBATTLEMANAGER);
        DWORD  oldProt = 0;
        // PAGE_READWRITE, not PAGE_WRITECOPY: the latter is only meaningful
        // for file-mapped regions and silently fails on .rdata under Wine,
        // leaving the vtable unpatched and the hook silently dead.
        if (!VirtualProtect(vtbl, 16 * sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
            sfe::log("ERROR: VirtualProtect on vtable failed (GLE=%lu)", GetLastError());
            writeStatus("failed", "cannot unprotect BattleManager vtable");
            return false;
        }

        s_origBattleDestruct = reinterpret_cast<decltype(s_origBattleDestruct)>(
                                   vtbl[VTBL_DESTRUCT_IDX]);
        vtbl[VTBL_DESTRUCT_IDX] = reinterpret_cast<DWORD>(HookedBattleDestruct);

        DWORD tmp = 0;
        VirtualProtect(vtbl, 16 * sizeof(DWORD), oldProt, &tmp);
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

        m_vtable_hooked = true;
        sfe::log("BattleManager destructor hooked");
    }

    // --- key-check hook: how the menus get driven -----------------------
    {
        void* t = reinterpret_cast<void*>(ADDR_CHECK_KEY_ONESHOT);
        void* tramp = installJmpHook(t, reinterpret_cast<void*>(&HookedCheckKeyOneshot),
                                     m_checkkey_saved);
        if (!tramp) {
            sfe::log("ERROR: could not hook checkKeyOneshot at %p", t);
            writeStatus("failed", "checkKeyOneshot hook failed");
            return false;
        }
        s_origCheckKey = reinterpret_cast<PFN_checkKeyOneshot>(tramp);
        m_checkkey_hooked = true;
        sfe::log("checkKeyOneshot hooked at %p (menus driven through it)", t);
    }

    if (!getOGLHook().install(m_encoder.get(), &SessionTagFn, this)) {
        sfe::log("ERROR: OGLHook::install failed — is opengl32.dll loaded?");
        writeStatus("failed", "OGL hook install failed");
        return false;
    }
    m_hooks_installed = true;
    sfe::log("OGL hook installed");

    transitionTo(AutoState::WAIT_TITLE);
    sfe::log("Session::init complete");
    return true;
}

// -------------------------------------------------------------------------
void Session::shutdown() {
    if (!m_hooks_installed && m_state == AutoState::IDLE) return;
    sfe::log("Session::shutdown begin");

    m_capturing = false;
    getOGLHook().uninstall();

    if (m_limiter_removed) restoreFrameLimiter();

    // Restore the vtable.  The old code never did this: it set
    // m_hooks_installed=false and left the game's vtable pointing at a
    // function inside a DLL that was about to be unmapped, so anything that
    // destroyed a BattleManager after unload jumped into freed memory.  There
    // are 12 crash dumps in the tree consistent with that.
    if (m_checkkey_hooked) {
        void* t = reinterpret_cast<void*>(ADDR_CHECK_KEY_ONESHOT);
        DWORD oldProt = 0;
        if (VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(t, m_checkkey_saved, 5);
            VirtualProtect(t, 5, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), t, 5);
            sfe::log("checkKeyOneshot restored");
        }
        m_checkkey_hooked = false;
    }

    if (m_vtable_hooked && s_origBattleDestruct) {
        DWORD* vtbl    = reinterpret_cast<DWORD*>(VTBL_CBATTLEMANAGER);
        DWORD  oldProt = 0;
        if (VirtualProtect(vtbl, 16 * sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
            vtbl[VTBL_DESTRUCT_IDX] = reinterpret_cast<DWORD>(s_origBattleDestruct);
            DWORD tmp = 0;
            VirtualProtect(vtbl, 16 * sizeof(DWORD), oldProt, &tmp);
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
            sfe::log("BattleManager vtable restored");
        } else {
            sfe::log("WARNING: could not restore vtable (GLE=%lu)", GetLastError());
        }
        m_vtable_hooked = false;
    }

    if (m_encoder) {
        m_encoder->stop();
        m_encoder.reset();
    }

    m_state           = AutoState::IDLE;
    m_hooks_installed = false;
    sfe::log("Session::shutdown complete");
}

// -------------------------------------------------------------------------
// onFrame — the single per-frame entry point (game thread)
// -------------------------------------------------------------------------
FrameTag Session::onFrame() {
    FrameTag tag{ false, 0, 0, 0 };

    // Start the encoder on first frame. This is the game thread with no loader
    // lock held, which is the only safe place to create the encoder thread --
    // see the note in init(). Doing it here also means FFmpeg gets connected
    // while the menus are still being navigated, so the FIFO is ready well
    // before the battle starts.
    if (!m_encoder_started && m_state != AutoState::IDLE) {
        m_encoder_started = true;
        if (!ensureEncoderStarted()) {
            finish(AutoState::FAILED, "VideoEncoder::start failed");
            return tag;
        }
    }

    switch (m_state) {

    case AutoState::IDLE:
    case AutoState::DONE:
    case AutoState::FAILED:
        break;

    // ------------------------------------------------------------------
    case AutoState::WAIT_TITLE: {
        // Wait for the game to actually REACH the title screen before sending
        // any keys. This was a fixed 10 s wait, which is wrong in both
        // directions: on fast hardware it wastes time, and under llvmpipe in a
        // CPU-capped container the game is still on the logo (scene 0) at 10 s,
        // so the entire navigation sequence gets fired into the opening
        // cutscene and the replay is never started.
        //
        // The scene id is observable, so gate on it rather than guessing.
        const int scene = currentScene();
        if (scene == SCENE_TITLE) {
            sfe::log("Title reached after %u ms — starting navigation", elapsedMs());
        
            transitionTo(AutoState::ENTER_REPLAY_MENU);
        } else if (elapsedMs() >= MS_TITLE_DEADLINE) {
            // Still not at the title. Proceed anyway rather than failing: some
            // configurations skip the logo entirely, and a wrong guess here is
            // recoverable where giving up is not.
            sfe::log("WARNING: title not reached after %u ms (scene=%d) — "
                     "starting navigation anyway", elapsedMs(), scene);
            transitionTo(AutoState::ENTER_REPLAY_MENU);
        }
        break;
    }

    // ------------------------------------------------------------------
    case AutoState::ENTER_REPLAY_MENU: {
        // Feed one key per step through the hooked checkKeyOneshot. A step is
        // complete once the game has actually consumed the injection
        // (s_inject_dik back to 0), so this self-paces to the game's polling
        // rather than guessing at timings -- the failure mode that dogged both
        // the old frame-count script and its wall-clock replacement.
        if (m_nav_step >= countof(k_nav)) {
            sfe::log("Navigation sequence delivered — waiting for battle");
            transitionTo(AutoState::START_REPLAY);
            break;
        }

        const NavStep& step = k_nav[m_nav_step];
        const uint32_t waited = GetTickCount() - m_state_tick;

        if (!m_nav_armed) {
            // Arm this step's injection. Do NOT advance here -- the step is
            // only finished once the game has actually consumed it. Advancing
            // on arm (the original bug) made every second step be skipped
            // without ever being offered to the game.
            if (waited >= step.delay_ms) {
                s_inject_dik = step.dik;
                m_nav_armed  = true;
                sfe::log("Nav %d/%d: armed DIK 0x%02X (scene=%d)",
                         m_nav_step + 1, countof(k_nav), step.dik, currentScene());
                m_state_tick = GetTickCount();
            }
        } else if (s_inject_dik == 0) {
            // Consumed by the game: this step really happened.
            sfe::log("Nav %d/%d: CONSUMED (scene=%d)",
                     m_nav_step + 1, countof(k_nav), currentScene());
            m_nav_armed = false;
            ++m_nav_step;
            m_state_tick = GetTickCount();
        } else if (waited > 3000) {
            // The game never asked about this key. Either the menu does not
            // poll it here, or the hook is not on the path this screen uses.
            sfe::log("Nav %d/%d: DIK 0x%02X never consumed after 3s (scene=%d)",
                     m_nav_step + 1, countof(k_nav), step.dik, currentScene());
            s_inject_dik = 0;
            m_nav_armed  = false;
            ++m_nav_step;
            m_state_tick = GetTickCount();
        }
        break;
    }

    // ------------------------------------------------------------------
    case AutoState::START_REPLAY:
        // Wait for the game to actually enter a battle. This is the one
        // transition we can observe, so it is the one we gate on.
        if (currentScene() == SCENE_BATTLE) {
            if (m_encoder->failed()) {
                finish(AutoState::FAILED, "encoder could not open its outputs");
                break;
            }
            // Do not arm capture until the encoder has somewhere to put the
            // frames, or the ring fills and back-pressures the game thread.
            if (!m_encoder->isFifoReady()) {
                if (elapsedMs() > MS_FIFO_DEADLINE) {
                    finish(AutoState::FAILED, "timed out waiting for FFmpeg to attach");
                }
                break;
            }

            m_frame_index = 0;
            if (m_cfg.fast_forward && !m_limiter_removed) removeFrameLimiter();

            m_capturing = true;
            transitionTo(AutoState::EXTRACTING);
            sfe::log("Battle reached after %u ms — capturing", elapsedMs());
        } else if ((elapsedMs() - m_state_tick_logged) >= 5000) {
            // Log where the game actually is while we wait. Without this the
            // only evidence of a failed navigation is "never reached battle",
            // which cannot distinguish "still at the title" from "sitting in
            // the wrong menu" from "playing but the scene id is unexpected".
            m_state_tick_logged = elapsedMs();
            sfe::log("START_REPLAY: waiting for battle (scene=%d, t=%u ms)",
                     currentScene(), elapsedMs());
        }
        if (m_state == AutoState::START_REPLAY && elapsedMs() > MS_BATTLE_DEADLINE) {
            // No retry. The old code re-ran the entire key script from a game
            // that was no longer at the title screen, firing 13 more presses
            // into an arbitrary menu -- a livelock, not a recovery. The runner
            // is a better place to decide about retries: it can start from a
            // known-clean process.
            finish(AutoState::FAILED, "never reached battle scene before deadline");
        }
        break;

    // ------------------------------------------------------------------
    case AutoState::EXTRACTING: {
        if (currentScene() != SCENE_BATTLE) {
            sfe::log("Scene left battle at frame %d", m_frame_index);
            finish(AutoState::DRAINING, nullptr);
            break;
        }
        if (m_frame_index >= MAX_FRAMES_PER_REPLAY) {
            sfe::log("Hit MAX_FRAMES_PER_REPLAY (%d)", MAX_FRAMES_PER_REPLAY);
            finish(AutoState::DRAINING, nullptr);
            break;
        }

        // Read both players' inputs for the tick being presented.
        uint16_t p1 = 0, p2 = 0;
        if (void* bm = *reinterpret_cast<void**>(ADDR_BATTLE_MANAGER)) {
            void* p1obj = *reinterpret_cast<void**>(
                              reinterpret_cast<char*>(bm) + BM_PLAYER1_OFFSET);
            void* p2obj = *reinterpret_cast<void**>(
                              reinterpret_cast<char*>(bm) + BM_PLAYER2_OFFSET);
            p1 = readPlayerInput(p1obj);
            p2 = readPlayerInput(p2obj);
        }

        tag.capture     = true;
        tag.frame_index = m_frame_index;
        tag.p1_input    = p1;
        tag.p2_input    = p2;
        ++m_frame_index;

        // --- throughput logging ---
        ++m_frames_since_log;
        {
            const uint32_t now_ms = GetTickCount();
            const uint32_t dt     = now_ms - m_last_log_tick;
            if (dt >= 5000) {
                sfe::log("Perf: %.1f fps, ring %d/%d, encoder wrote %d",
                         m_frames_since_log * 1000.0f / static_cast<float>(dt),
                         m_encoder->ring().pendingCount(), RING_CAPACITY,
                         m_encoder->totalFramesWritten());
                m_frames_since_log = 0;
                m_last_log_tick    = now_ms;
            }
        }
        break;
    }

    // ------------------------------------------------------------------
    case AutoState::DRAINING:
        // Capture is already disarmed. Give the encoder a moment to write out
        // what is still in the ring, then stop it (which flushes and closes
        // the FIFO, letting FFmpeg finalise the mp4) and exit.
        if (GetTickCount() - m_state_tick >= MS_DRAIN) {
            sfe::log("Drain complete — %d frames captured", m_frame_index);
            if (m_limiter_removed) restoreFrameLimiter();
            if (m_encoder) m_encoder->stop();

            const int written = m_encoder ? m_encoder->totalFramesWritten() : 0;
            if (written <= 0) {
                writeStatusAndExit("failed", "no frames were written");
            }
            writeStatusAndExit("ok", nullptr);
        }
        break;
    }

    return tag;
}

// -------------------------------------------------------------------------
bool Session::ensureEncoderStarted() {
    char csv_path[SFE_PATH_MAX];
    snprintf(csv_path, sizeof(csv_path), "%s/inputs.csv", m_cfg.output_dir);
    sfe::log("Starting encoder (game thread) — csv=%s", csv_path);
    return m_encoder && m_encoder->start(m_cfg, csv_path);
}

// -------------------------------------------------------------------------
void Session::onBattleEnd() {
    if (m_state == AutoState::EXTRACTING) {
        sfe::log("BattleManager destroyed at frame %d", m_frame_index);
        finish(AutoState::DRAINING, nullptr);
    }
}

// -------------------------------------------------------------------------
void Session::finish(AutoState terminal, const char* reason) {
    // Disarm capture immediately. This is the fix for the largest corruption
    // in the old output: capture stayed armed while the FSM was not producing
    // frame indices, so every subsequent presented frame -- menus, result
    // screens, thousands of them at unthrottled speed -- was recorded carrying
    // the last gameplay index.
    m_capturing = false;

    if (reason) sfe::log("Session failing: %s", reason);
    transitionTo(terminal);

    if (terminal == AutoState::FAILED) {
        if (m_limiter_removed) restoreFrameLimiter();
        if (m_encoder) m_encoder->stop();
        writeStatusAndExit("failed", reason ? reason : "unspecified failure");
    }
}

// -------------------------------------------------------------------------
void Session::transitionTo(AutoState next) {
    sfe::log("FSM: %s -> %s  (t=%u ms)", toString(m_state), toString(next),
             elapsedMs());
    m_state      = next;
    m_state_tick = GetTickCount();
}

// -------------------------------------------------------------------------
// writeStatusAndExit
// -------------------------------------------------------------------------
// The old build had no terminal state at all: after the last replay it stopped
// the encoder and dropped to IDLE with the process still running. A container
// job that never exits cannot be orchestrated, so the runner had nothing to
// wait on and no way to tell success from a hang.
void Session::writeStatus(const char* status, const char* reason) {
    const int frames = m_encoder ? m_encoder->totalFramesWritten() : 0;

    sfe::log("=== RESULT: %s (%s) — %d frames, %u ms ===",
             status, reason ? reason : "-", frames, elapsedMs());

    char esc_replay[SFE_PATH_MAX * 2] = {};
    char esc_reason[512] = {};
    json_escape(m_replay_name, esc_replay, sizeof(esc_replay));
    if (reason) json_escape(reason, esc_reason, sizeof(esc_reason));

    if (FILE* f = fopen(m_cfg.status_path, "w")) {
        fprintf(f,
                "{\n"
                "  \"status\": \"%s\",\n"
                "  \"replay\": \"%s\",\n"
                "  \"frames\": %d,\n"
                "  \"elapsed_ms\": %u,\n"
                "  \"reason\": %s%s%s\n"
                "}\n",
                status,
                esc_replay,
                frames,
                elapsedMs(),
                reason ? "\"" : "null",
                reason ? esc_reason : "",
                reason ? "\"" : "");
        fclose(f);
    } else {
        sfe::log("WARNING: could not write status file %s", m_cfg.status_path);
    }
}

void Session::writeStatusAndExit(const char* status, const char* reason) {
    const bool ok = (std::strcmp(status, "ok") == 0);
    writeStatus(status, reason);
    sfe::closeLog();

    // ExitProcess rather than a clean unwind: we are on the game thread inside
    // a rendering callback, with hooks installed in the game's own code. There
    // is no safe way to unwind out of here, and everything that needed
    // flushing (encoder, CSV, FIFO, log) has been flushed above.
    //
    // MUST NOT be called from Initialize() -- that runs under the loader lock,
    // and terminating there reads to SWRSToys as a crash during module load.
    ExitProcess(ok ? 0u : 1u);
}

// =========================================================================
// Replay discovery
// =========================================================================
bool Session::findStagedReplay() {
    // FindFirstFile rather than std::filesystem: <filesystem> is one of the
    // heaviest msvcp140 dependencies in the STL, and all this needs is "the
    // one .rep in a flat directory".
    char pattern[SFE_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*.rep", m_cfg.replay_dir);

    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        sfe::log("ERROR: no .rep files in %s (GLE=%lu)",
                 m_cfg.replay_dir, GetLastError());
        return false;
    }

    int count = 0;
    char first[SFE_PATH_MAX] = {};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (count == 0) {
            strncpy(first, fd.cFileName, sizeof(first) - 1);
            first[sizeof(first) - 1] = '\0';
        }
        ++count;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (count == 0) {
        sfe::log("ERROR: no .rep files in %s", m_cfg.replay_dir);
        return false;
    }
    if (count > 1) {
        // Not fatal, but it means the runner staged incorrectly and the
        // output would no longer be attributable to a known input file.
        sfe::log("WARNING: %d .rep files staged; expected exactly 1. "
                 "Using '%s': identity may be wrong.", count, first);
    }

    strncpy(m_replay_name, first, sizeof(m_replay_name) - 1);
    m_replay_name[sizeof(m_replay_name) - 1] = '\0';
    return true;
}

// =========================================================================
// Input reading
// =========================================================================
uint16_t Session::readPlayerInput(void* char_obj) const {
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
// Frame limiter
// =========================================================================
void Session::removeFrameLimiter() {
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

void Session::restoreFrameLimiter() {
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
