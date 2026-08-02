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

// =========================================================================
// The scene machine
// =========================================================================
// Read off the main loop at 0x00407F11..0x00407F9D.  The application object
// lives at 0x0089FF90 and the loop is, in full:
//
//     if (app->sceneId == app->newSceneId) {          // 0x8A0044 vs 0x8A0040
//         app->newSceneId = app->scene->onProcess();  // vtable slot 1
//         if (app->newSceneId != app->sceneId)
//             CreateThread(loadGraphics);             // 0x00408410
//     } else if (!app->loadingScreen->isBusy()) {
//         commitSceneSwap();                          // 0x00407C50
//     }
//
// Three facts matter, and all three are why the earlier bypass crashed:
//
//   1. A scene change is *requested by returning the new scene id from
//      onProcess*.  Nothing else is a supported way to ask.
//   2. The engine starts the loader thread itself, but only on the tick where
//      onProcess returned a different id.  Poking newSceneId from outside that
//      window means the engine never starts the loader (so we had to spawn it
//      by hand, racing the engine) *and* the title screen's own onProcess
//      overwrites the value on the very next tick.
//   3. onProcess runs on the game's main thread at a defined point in the
//      loop.  Anything that mutates battle state has to happen there.
//
// So the bypass hooks vtable slot 1 of whatever scene is current, and returns
// the scene id we want from inside it.  That is byte-for-byte what pressing
// the button in the menu would have done -- same thread, same call site, same
// return path -- with the menu's own handler code inlined into the hook.
constexpr DWORD ADDR_SCENE_OBJECT = 0x008A000C;  // app->scene   (app + 0x7C)
constexpr int   VTBL_SCENE_ONPROCESS = 1;        // int __thiscall onProcess()

static inline void* currentSceneObject() {
    return *reinterpret_cast<void* volatile*>(ADDR_SCENE_OBJECT);
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
// Both are bounds on pathology, not pacing -- the FSM polls the scene id and
// moves the instant it changes. They are generous because they have to cover
// the *loaded* case: a single capture reaches the battle in 12 s, but with ten
// workers sharing the machine the same boot took 40 s, and a deadline tuned to
// the idle case fails replays that were about to work.
constexpr uint32_t MS_TITLE_DEADLINE  = 120000;
constexpr uint32_t MS_BATTLE_DEADLINE = 240000; // start armed -> battle, else fail
constexpr uint32_t MS_DRAIN          = 1500;   // post-match settle before stop
constexpr uint32_t MS_FIFO_DEADLINE  = 60000;  // wait for FFmpeg to attach

// =========================================================================
// Starting a replay without touching the menus
// =========================================================================
// No synthetic input anywhere.  Every attempt to deliver a keystroke failed,
// because Soku reads the keyboard through DirectInput and Wine's dinput reads
// real evdev devices -- not Wine's synthetic Win32 queue, and not the X
// server's synthetic events.  SendInput, SendInput+SetFocus, SendInput with a
// window manager, PostMessage(WM_KEYDOWN), xdotool/XTEST and a /dev/uinput
// virtual keyboard were all tried; the game rendered its title screen happily
// while the scene id never moved.  Hooking the game's own checkKeyOneshot()
// (0x0043DE30) did not help either: it was never called once.
//
// So do not press the button.  Do what the button does.
//
// The replay list's confirm handler is at 0x0044B3D0 and is short:
//
//     sprintf(path, "%s/%s", dir, filename);          // 0x00858370 = "%s/%s"
//     if (!getInputManager()->readReplay(path))       // 0x0042EAC0
//         return;                                     //   bad .rep, stay put
//     playSound(0x28);
//     g_requestedScene = SCENE_LOADING;               // 0x00882A94 = 6
//
// and the menu scene's onProcess then translates that request (0x004276D0,
// case 6 at 0x00427786) into the actual transition:
//
//     mode = getInputManager()->replayBattleMode;     // this + 0xEC
//     setBattleMode(mode == 0 ? 0 : mode == 7 ? 7 : 3, 2);   // 0x0043E9A0
//     return SCENE_LOADING;
//
// Both halves are reproduced below, in that order, inside a hook on the
// current scene's onProcess.
//
// WHY THE PREVIOUS ATTEMPT CRASHED
// --------------------------------
// It called readReplay(), wrote mainMode/subMode as two loose bytes, poked
// newSceneId from the render thread and spawned the loader thread by hand.
// The game reached the loading screen and then faulted at 0x004386A6 --
// `mov ecx,[0x008985E4]; mov eax,[ecx]` with the BattleManager still null.
//
// Two separate reasons, both visible in the disassembly:
//
//   * setBattleMode is not two byte stores.  It initialises about twenty
//     globals -- 0x00899D08 (which is handed to the battle worker task that
//     faulted, as its argument), 0x00899D0C/10/30/54..59, 0x00898678..90,
//     and 0x00899CEC.  Writing two of them and leaving the rest stale is what
//     the battle creator then walked into.
//   * The scene request has to come back from onProcess.  Written from
//     outside, the title screen's own onProcess overwrote it on the next tick,
//     and the engine's loader-thread launch (which only runs on the tick where
//     onProcess changed the id) never fired -- hence the hand-rolled thread,
//     racing whatever value newSceneId happened to hold when it read it.
//
// Doing it from inside onProcess removes both: same thread, same call site,
// same return path the menu would have used, with the game's own setup
// function doing the setup.
constexpr DWORD ADDR_INPUT_MANAGER     = 0x00898718;  // SokuLib inputMgr
constexpr DWORD ADDR_READ_REPLAY       = 0x0042EAC0;  // InputManager::readReplay
constexpr DWORD ADDR_SET_BATTLE_MODE   = 0x0043E9A0;  // setBattleMode(main, sub)

// -------------------------------------------------------------------------
// The input-device list, and why setBattleMode trips over it in a container
// -------------------------------------------------------------------------
// setBattleMode picks the input profile for the local player:
//
//     sel = inputMgrCluster->selectedDevice;      // 0x0040A8E0: [ecx + 0x74]
//     g_profile = (sel < 0) ? &defaultProfile     // 0x008986A8
//                           : deviceProfiles.at(sel);
//
// deviceProfiles (0x00899CEC) is a std::vector of 0x68-byte entries built by
// enumerating DirectInput devices (0x00442CC5), and .at() is the checked
// accessor -- 0x0043E3B0 calls the CRT's invalid-argument handler, which
// raises 0xC000000D, if the index is past the end.
//
// In a container there are no input devices at all: no /dev/input, so Wine's
// dinput enumerates nothing and the vector is empty.  The selector is still
// 0 ("first device"), so .at(0) on an empty vector kills the process.  That
// is the same root cause that makes every synthetic-keystroke approach fail,
// surfacing somewhere completely different -- not "keys do not arrive" but
// "the device list is empty".
//
// The game already has the answer in its own code: a negative selector means
// "no device, use the default profile".  For replay playback that is exactly
// right, because both players' inputs come from the .rep stream and no device
// is ever read.  So when -- and only when -- the index would be out of range,
// set the selector negative first.  On a machine with real input devices this
// changes nothing.
constexpr DWORD ADDR_INPUT_CLUSTER      = 0x0089A248;  // SokuLib inputMgrs
constexpr int   CLUSTER_SELECTED_DEVICE = 0x74;        // signed char
constexpr DWORD ADDR_DEVICE_PROFILES    = 0x00899CEC;  // std::vector<Profile>
constexpr int   DEVICE_PROFILE_SIZE     = 0x68;
constexpr signed char NO_INPUT_DEVICE   = -2;          // the game's own 0xFE

// Number of enumerated input devices, read the way .at() reads it.
static int deviceProfileCount() {
    auto* v = reinterpret_cast<DWORD volatile*>(ADDR_DEVICE_PROFILES);
    const DWORD first = v[1], last = v[2];
    if (!first || last < first) return 0;
    return static_cast<int>((last - first) / DEVICE_PROFILE_SIZE);
}

// Returns true if it had to intervene.
static bool guardInputDeviceSelector() {
    auto* sel = reinterpret_cast<volatile signed char*>(ADDR_INPUT_CLUSTER
                                                        + CLUSTER_SELECTED_DEVICE);
    const int count = deviceProfileCount();
    const int want  = *sel;

    sfe::log("Input devices: %d enumerated, selector=%d", count, want);
    if (want < 0 || want < count) return false;   // the game's own path is safe

    *sel = NO_INPUT_DEVICE;
    sfe::log("No usable input device (%d enumerated, selector was %d) — "
             "selecting the default profile so setBattleMode cannot fault. "
             "Replay inputs come from the .rep, so no device is read.",
             count, want);
    return true;
}

// Battle mode recorded in the .rep header, as readReplay() decoded it.  The
// game branches on this to pick mainMode, because a story-mode replay and a
// versus replay need different battle setups.
constexpr int IM_REPLAY_MODE_OFFSET = 0xEC;

constexpr int BATTLE_SUBMODE_REPLAY = 2;

using PFN_readReplay    = bool (__thiscall*)(void* self, const char* path);
using PFN_setBattleMode = void (__cdecl*)(int mainMode, int subMode);

// onProcess is __thiscall with no arguments, which MSVC cannot spell for a
// free function.  __fastcall is the same thing with EDX additionally live:
// ECX carries `this`, no arguments touch the stack, and the callee cleans
// nothing -- so a plain `ret` on both sides matches.
using PFN_sceneProcess = int (__fastcall*)(void* self, void* edx);

static PFN_sceneProcess s_orig_scene_process = nullptr;
static DWORD*           s_scene_vtbl         = nullptr;

// Set up by the FSM before the hook is armed, read inside it.
static char s_replay_path[SFE_PATH_MAX] = {};

// Hook <-> FSM handshake.  Both sides run on the game thread in practice, but
// the OGL hook is only *believed* to share it, so treat these as cross-thread.
static volatile LONG s_start_request = 0;  // 1: do the start on the next tick
static volatile LONG s_start_done    = 0;  // 1: ok, -1: the game rejected the .rep
static volatile LONG s_start_mode    = -1; // mainMode the game picked, for the log

// The hook.  Runs on the game's main thread from 0x00407F43.
static int __fastcall HookedSceneProcess(void* This, void* edx) {
    if (InterlockedCompareExchange(&s_start_request, 0, 1) == 1) {
        auto readReplay = reinterpret_cast<PFN_readReplay>(ADDR_READ_REPLAY);
        void* mgr = reinterpret_cast<void*>(ADDR_INPUT_MANAGER);

        if (!readReplay(mgr, s_replay_path)) {
            // The game itself says this file is not loadable.  Report it as
            // such rather than letting it look like a navigation failure --
            // that distinction was impossible to make with keypresses.
            InterlockedExchange(&s_start_done, -1);
            return s_orig_scene_process(This, edx);
        }

        guardInputDeviceSelector();

        const unsigned char rep_mode =
            *reinterpret_cast<volatile unsigned char*>(ADDR_INPUT_MANAGER
                                                       + IM_REPLAY_MODE_OFFSET);
        const int main_mode = (rep_mode == 0) ? 0 : (rep_mode == 7 ? 7 : 3);

        reinterpret_cast<PFN_setBattleMode>(ADDR_SET_BATTLE_MODE)(
            main_mode, BATTLE_SUBMODE_REPLAY);

        InterlockedExchange(&s_start_mode, main_mode);
        InterlockedExchange(&s_start_done, 1);
        return SCENE_LOADING;
    }
    return s_orig_scene_process(This, edx);
}

// Patch slot 1 of the *current* scene's vtable.  The scene object is a
// singleton of its class while it is current, and the vtable is restored on
// shutdown, so nothing outlives the module.
static bool installSceneHook() {
    void* scene = currentSceneObject();
    if (!scene) {
        sfe::log("ERROR: no current scene object at 0x%08lX", ADDR_SCENE_OBJECT);
        return false;
    }

    DWORD* vtbl = *reinterpret_cast<DWORD**>(scene);
    if (!vtbl) {
        sfe::log("ERROR: scene %p has a null vtable", scene);
        return false;
    }

    DWORD oldProt = 0;
    // PAGE_READWRITE, not PAGE_WRITECOPY -- the latter silently fails on
    // .rdata under Wine and leaves the patch unapplied (see the BattleManager
    // vtable hook, which hit exactly that).
    if (!VirtualProtect(vtbl, 8 * sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
        sfe::log("ERROR: VirtualProtect on scene vtable %p failed (GLE=%lu)",
                 vtbl, GetLastError());
        return false;
    }

    s_scene_vtbl         = vtbl;
    s_orig_scene_process = reinterpret_cast<PFN_sceneProcess>(
                               vtbl[VTBL_SCENE_ONPROCESS]);
    vtbl[VTBL_SCENE_ONPROCESS] = reinterpret_cast<DWORD>(HookedSceneProcess);

    DWORD tmp = 0;
    VirtualProtect(vtbl, 8 * sizeof(DWORD), oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

    sfe::log("Scene onProcess hooked: scene=%p vtbl=%p orig=%p",
             scene, vtbl, reinterpret_cast<void*>(s_orig_scene_process));
    return true;
}

static void restoreSceneHook() {
    if (!s_scene_vtbl || !s_orig_scene_process) return;
    DWORD oldProt = 0;
    if (VirtualProtect(s_scene_vtbl, 8 * sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
        s_scene_vtbl[VTBL_SCENE_ONPROCESS] =
            reinterpret_cast<DWORD>(s_orig_scene_process);
        DWORD tmp = 0;
        VirtualProtect(s_scene_vtbl, 8 * sizeof(DWORD), oldProt, &tmp);
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        sfe::log("Scene onProcess restored");
    }
    s_scene_vtbl         = nullptr;
    s_orig_scene_process = nullptr;
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

    // The scene hook is NOT installed here.  It patches the vtable of whatever
    // scene is current, and at Initialize() time that is the logo -- a class
    // that gets destroyed before it would ever be asked for a scene change.
    // WAIT_TITLE installs it once the title screen is up.

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
    if (m_scene_hooked) {
        restoreSceneHook();
        m_scene_hooked = false;
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
        // Wait for the game to actually REACH the title screen.  Not a fixed
        // delay: under llvmpipe in a CPU-capped container the game is still on
        // the logo (scene 0) well past ten seconds, and the scene id is
        // observable, so gate on it.
        //
        // The title is where the scene hook goes in.  Earlier is not safe --
        // the logo and opening scenes are separate objects with their own
        // vtables, and hooking one of those would patch a class that is about
        // to be destroyed and never asked for a scene change again.
        const int scene = currentScene();
        if (scene != SCENE_TITLE) {
            if (elapsedMs() >= MS_TITLE_DEADLINE) {
                finish(AutoState::FAILED, "never reached the title screen");
            }
            break;
        }

        sfe::log("Title reached after %u ms (scene=%d)", elapsedMs(), scene);
        if (!installSceneHook()) {
            finish(AutoState::FAILED, "could not hook the scene's onProcess");
            break;
        }
        m_scene_hooked = true;
        transitionTo(AutoState::ENTER_REPLAY_MENU);
        break;
    }

    // ------------------------------------------------------------------
    case AutoState::ENTER_REPLAY_MENU: {
        // Ask the hook to perform the start on the game's next logic tick,
        // then wait for its verdict.  Nothing happens on this thread: the
        // whole point is that readReplay() and setBattleMode() run where the
        // game runs them.
        if (!m_start_armed) {
            snprintf(s_replay_path, sizeof(s_replay_path), "%s/%s",
                     m_cfg.replay_dir, m_replay_name);
            // The game's own sprintf format is "%s/%s" (0x00858370), so
            // forward slashes are what readReplay() is used to seeing.  Wine
            // accepts either, but matching the game removes a variable.
            for (char* p = s_replay_path; *p; ++p)
                if (*p == '\\') *p = '/';

            sfe::log("Starting replay programmatically: %s", s_replay_path);
            InterlockedExchange(&s_start_request, 1);
            m_start_armed = true;
            m_state_tick  = GetTickCount();
            break;
        }

        const LONG done = InterlockedCompareExchange(&s_start_done, 0, 0);
        if (done == 1) {
            sfe::log("readReplay accepted the file; setBattleMode(%ld, %d) done "
                     "— scene requested", InterlockedCompareExchange(&s_start_mode, 0, 0),
                     BATTLE_SUBMODE_REPLAY);
            transitionTo(AutoState::START_REPLAY);
        } else if (done == -1) {
            // The game read the header and refused it.  This is a property of
            // the .rep, not of our timing, so retrying cannot help.
            finish(AutoState::FAILED, "the game rejected the .rep file");
        } else if (GetTickCount() - m_state_tick > 10000) {
            // onProcess never ran, which means the hook is not on the path the
            // engine actually calls -- report that rather than timing out
            // later against the battle deadline with no explanation.
            finish(AutoState::FAILED,
                   "scene onProcess was never called after arming the start");
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
        } else {
            // Log where the game actually is while we wait. Without this the
            // only evidence of a failed start is "never reached battle", which
            // cannot distinguish "still at the title" from "stuck in the
            // loading scene" from "playing but the scene id is unexpected".
            const int scene = currentScene();
            if (scene != m_last_scene_logged) {
                m_last_scene_logged = scene;
                sfe::log("START_REPLAY: scene -> %d (t=%u ms)", scene, elapsedMs());
            } else if ((elapsedMs() - m_state_tick_logged) >= 5000) {
                m_state_tick_logged = elapsedMs();
                sfe::log("START_REPLAY: waiting for battle (scene=%d, t=%u ms)",
                         scene, elapsedMs());
            }
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
