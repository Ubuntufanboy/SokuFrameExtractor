// =========================================================================
// SokuFrameExtractor — session.cpp
// =========================================================================
// See session.hpp for why this is one-replay-per-process.
// =========================================================================

#include <winsock2.h>
#include <windows.h>

#include "sfe/session.hpp"
#include "sfe/logger.hpp"
#include "sfe/player_state.hpp"

#include <shlwapi.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace sfe {

static void scanForNeedle();
static void applyBranch();
static void forceBranchInput(void*, void*, long);
static long branchScanFrame();

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

// BattleManager + 0x004, from third_party/SokuLib/src/BattleManager.hpp. Its
// immediate neighbours at 0x00C/0x010 are the two character managers this file
// already dereferences correctly every frame, so the struct layout is not in
// question here -- only whether this particular field is the counter, which
// pipeline/verify_state.py checks by requiring it to advance monotonically.
constexpr int BM_FRAME_COUNT_OFFSET = 0x04;
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

// -------------------------------------------------------------------------
// Finding the decoded replay input buffer
// -------------------------------------------------------------------------
// The counterfactual measurement this project needs -- roll the SAME state
// forward under two different actions -- cannot come from the corpus, because
// the corpus never contains one state played two ways. It can come from the
// game: play a replay twice and overwrite one player's input for a window of
// frames in the second run. Everything before the window is bit-identical
// because replay playback is deterministic, so the difference after it is a
// true interventional effect with the opponent's inputs held fixed.
//
// Editing the .rep file is the hard way round: its input section is deflate
// compressed and `pipeline/repparse.py` fails to decode two thirds of the
// corpus. readReplay() has already done that work in memory, so the buffer it
// produced is what to patch -- once its address is known, which is what this
// searches for.
//
// The needle comes from outside: `inputs.csv` for a replay already captured
// gives the exact per-frame words, and SokuLib documents the bit layout
// (BattleKeys: up/down/left/right/A/B/C/dash then A+B, B+C). So the search is
// for a byte string that must be present rather than for a plausible-looking
// pointer, which is the difference between finding the buffer and believing
// one has been found.
constexpr size_t MAX_NEEDLE = 1024;

static int hexNybble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Where the needle was found, so the branch patch can be placed relative to it.
static unsigned char* s_replay_inputs = nullptr;

// -------------------------------------------------------------------------
// The branch
// -------------------------------------------------------------------------
// Overwrite one player's input for a window of frames. Run the same replay
// twice -- once clean, once branched -- and everything before the window is
// bit-identical, because replay playback is deterministic. The difference
// after it is a true interventional effect with the opponent's inputs held
// fixed, which is exactly the counterfactual the corpus cannot contain and
// exactly the pairing `scripts/action_influence.py` computes inside the model.
//
// The layout is confirmed by the search rather than assumed: the needle is 48
// frames of INTERLEAVED p1/p2 16-bit words, 192 contiguous bytes, and it
// matched exactly once in the whole address space.
static void applyBranch() {
    if (!s_replay_inputs) return;
    const char* fr = getenv("SFE_BRANCH_FRAME");
    if (!fr || !*fr) return;
    const long frame  = strtol(fr, nullptr, 10);
    const char* le    = getenv("SFE_BRANCH_LEN");
    const char* wd    = getenv("SFE_BRANCH_WORD");
    const char* pl    = getenv("SFE_BRANCH_PLAYER");
    const long len    = le ? strtol(le, nullptr, 10) : 32;
    const long word   = wd ? strtol(wd, nullptr, 0)  : 0;
    const long player = pl ? strtol(pl, nullptr, 10) : 1;
    if (frame < 0 || len <= 0 || player < 1 || player > 2) {
        sfe::log("applyBranch: bad parameters, skipping");
        return;
    }
    // Frame 0 of the buffer, from the frame the needle was taken at.
    const char* nf = getenv("SFE_NEEDLE_FRAME");
    const long nfr = nf ? strtol(nf, nullptr, 10) : 0;
    auto* base = s_replay_inputs - 4 * nfr;
    auto* slot = reinterpret_cast<unsigned short*>(base + 4 * frame)
               + (player - 1);
    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(unsigned short) * 2 * len,
                        PAGE_READWRITE, &old_prot)) {
        sfe::log("applyBranch: VirtualProtect failed (GLE=%lu)", GetLastError());
        return;
    }
    unsigned short before = slot[0];
    for (long i = 0; i < len; ++i)
        slot[i * 2] = static_cast<unsigned short>(word);
    VirtualProtect(slot, sizeof(unsigned short) * 2 * len, old_prot, &old_prot);
    sfe::log("applyBranch: player %ld frames %ld..%ld set to 0x%04lX "
             "(was 0x%04X) at %p", player, frame, frame + len - 1,
             word, before, slot);
}

// Overwrite the character's own input word, and the KeymapManager copy the
// replay fills, for a window of battle frames.
//
//   char_obj + 0x754   the word `readPlayerInput` already reads -- what the
//                      character logic consumes this tick
//   char_obj + 0x750   -> KeyManager -> KeymapManager, whose `inKeys` SokuLib
//                      documents as the field replays and netplay "copy
//                      to/from"; +0x60 past the vtable, 13-int KeyBindings and
//                      10-int KeyInput
//
// Both, because which one the engine reads back is exactly what is unknown.

// `char_obj + 0x754` is NOT a 16-bit word. `readPlayerInput` reads it as
// SWRCHARINPUT -- eight ints {lr, ud, a, b, c, d, ch, s}, axes SIGNED. Writing
// a uint16 there sets only the low half of `lr`: with lr at 0xFFFFFFF8 (-8,
// left) a write of 0x0004 leaves 0xFFFF0004, still negative, still left. The
// write lands and means nothing, which is exactly what the first attempt
// measured -- identical inputs, identical state, no divergence.
struct SWRCHARINPUT { int lr, ud, a, b, c, d, ch, s; };

// -------------------------------------------------------------------------
// Injecting through the engine's own input source
// -------------------------------------------------------------------------
// Offsets confirmed by measurement, not read off the header. The mirror scan
// found p2's live input word at inputMgr+0xD0, and the character's own pointer
// chain (char+0x750 -> KeyManager -> KeymapManager) puts p2's KeymapManager at
// inputMgr+0x70. So inKeys sits at KeymapManager+0x60 -- which settles the
// contradiction inside SokuLib's header, where KeyInput is declared with ten
// ints but annotated `int[8] (32)` two lines above. Ten is right:
// 4 (vtable) + 0x34 (KeyBindings) + 0x28 (KeyInput) = 0x60 exactly. outKeys
// follows at +0x62, readInKeys at +0x64.
//
// Reached through the character rather than from a hardcoded address, so it
// stays correct if the managers ever move: the two the run measured were
// inputMgr+0x08 and inputMgr+0x70, 0x68 apart, but nothing here depends on
// that.
constexpr int CHAR_KEYMANAGER_OFFSET = 0x750;
constexpr int KMM_INKEYS_OFFSET      = 0x60;
constexpr int KMM_READIN_OFFSET      = 0x64;

static void* keymapManagerOf(void* char_obj) {
    if (!char_obj) return nullptr;
    auto* km = *reinterpret_cast<void**>(
                   reinterpret_cast<char*>(char_obj) + CHAR_KEYMANAGER_OFFSET);
    if (!km) return nullptr;
    return *reinterpret_cast<void**>(km);
}

// readInKeys is set alongside the word because that is the flag SokuLib names
// for "this manager's input arrives from elsewhere" -- the netplay path. Under
// it the engine's refill is the thing doing the writing to keyMap, which is
// why this works where writing keyMap directly could not: the refill happens
// after the hook either way, so the only way to win is to own its input.
// The two KeymapManagers, as static addresses. In replay submode the
// character's own chain resolves to exactly these (p1 -> inputMgr+0x08, p2 ->
// inputMgr+0x70), but in a PLAYING submode char+0x750 comes back null, so the
// chain cannot be the only way in. Measured both ways rather than assumed.
constexpr DWORD ADDR_KEYMAP_MGR_P1 = ADDR_INPUT_MANAGER + 0x08;
constexpr DWORD ADDR_KEYMAP_MGR_P2 = ADDR_INPUT_MANAGER + 0x70;
constexpr int   KMM_INPUT_OFFSET   = 0x38;   // KeyInput, ten ints

static void* keymapManagerFor(void* char_obj, int player) {
    if (void* kmm = keymapManagerOf(char_obj)) return kmm;
    return reinterpret_cast<void*>(player == 2 ? ADDR_KEYMAP_MGR_P2
                                               : ADDR_KEYMAP_MGR_P1);
}

// Three surfaces, because which one a PLAYING submode reads is exactly what is
// unknown, and writing all three costs nothing next to another run:
//
//   +0x38  KeyInput   the decoded ten-int form, what a device poll produces
//   +0x60  inKeys     the packed word netplay and replays copy through
//   +0x64  readInKeys the flag that says inKeys is the source
//
// SFE_DRIVE narrows it afterwards: "input", "inkeys", or unset for all.
static void forceViaKeymap(void* char_obj, int player, unsigned short word) {
    void* kmm = keymapManagerFor(char_obj, player);
    if (!kmm) return;
    auto* p = reinterpret_cast<char*>(kmm);
    const char* which = getenv("SFE_DRIVE");
    if (!which || strcmp(which, "input") == 0) {
        auto* in = reinterpret_cast<volatile int*>(p + KMM_INPUT_OFFSET);
        in[0] = (word & INPUT_LEFT) ? -1 : (word & INPUT_RIGHT) ? 1 : 0;
        in[1] = (word & INPUT_UP)   ? -1 : (word & INPUT_DOWN)  ? 1 : 0;
        in[2] = (word & INPUT_A)      ? 1 : 0;
        in[3] = (word & INPUT_B)      ? 1 : 0;
        in[4] = (word & INPUT_C)      ? 1 : 0;
        in[5] = (word & INPUT_D)      ? 1 : 0;
        in[6] = (word & INPUT_CHANGE) ? 1 : 0;
        in[7] = (word & INPUT_SPELL)  ? 1 : 0;
    }
    if (!which || strcmp(which, "inkeys") == 0) {
        *reinterpret_cast<volatile unsigned short*>(p + KMM_INKEYS_OFFSET) = word;
        *reinterpret_cast<volatile unsigned char*>(p + KMM_READIN_OFFSET)  = 1;
    }
}

// One-shot picture of both managers, so a null result can be told apart from a
// write that landed somewhere inert.
static void dumpKeymapManagers(void* p1obj, void* p2obj) {
    void* o[2] = { p1obj, p2obj };
    for (int i = 0; i < 2; ++i) {
        void* kmm = keymapManagerFor(o[i], i + 1);
        auto* p = reinterpret_cast<char*>(kmm);
        auto* in = reinterpret_cast<volatile int*>(p + KMM_INPUT_OFFSET);
        sfe::log("  p%d kmm=%p (chain=%p) input=[%d %d %d %d %d %d] "
                 "inKeys=0x%04X outKeys=0x%04X readInKeys=%d",
                 i + 1, kmm, keymapManagerOf(o[i]),
                 in[0], in[1], in[2], in[3], in[4], in[5],
                 *reinterpret_cast<volatile unsigned short*>(p + KMM_INKEYS_OFFSET),
                 *reinterpret_cast<volatile unsigned short*>(p + KMM_INKEYS_OFFSET + 2),
                 *reinterpret_cast<volatile unsigned char*>(p + KMM_READIN_OFFSET));
    }
}

static void forceOne(void* char_obj, unsigned short word) {
    if (!char_obj) return;
    auto* inp = reinterpret_cast<volatile SWRCHARINPUT*>(
        reinterpret_cast<char*>(char_obj) + CHAR_INPUT_OFFSET);
    inp->lr = (word & INPUT_LEFT) ? -1 : (word & INPUT_RIGHT) ? 1 : 0;
    inp->ud = (word & INPUT_UP)   ? -1 : (word & INPUT_DOWN)  ? 1 : 0;
    inp->a  = (word & INPUT_A)      ? 1 : 0;
    inp->b  = (word & INPUT_B)      ? 1 : 0;
    inp->c  = (word & INPUT_C)      ? 1 : 0;
    inp->d  = (word & INPUT_D)      ? 1 : 0;
    inp->ch = (word & INPUT_CHANGE) ? 1 : 0;
    inp->s  = (word & INPUT_SPELL)  ? 1 : 0;
}

static void forceBranchInput(void* p1obj, void* p2obj, long battle_frame) {
    const char* fr = getenv("SFE_BRANCH_FRAME");
    if (!fr || !*fr) return;
    const long start = strtol(fr, nullptr, 10);
    const char* le = getenv("SFE_BRANCH_LEN");
    const char* wd = getenv("SFE_BRANCH_WORD");
    const char* pl = getenv("SFE_BRANCH_PLAYER");
    const long len    = le ? strtol(le, nullptr, 10) : 32;
    const long word   = wd ? strtol(wd, nullptr, 0)  : 0;
    const long player = pl ? strtol(pl, nullptr, 10) : 1;
    void* obj = (player == 2) ? p2obj : p1obj;
    if (!obj) return;
    auto* slot = reinterpret_cast<volatile SWRCHARINPUT*>(
        reinterpret_cast<char*>(obj) + CHAR_INPUT_OFFSET);
    if (battle_frame == start + len) {
        sfe::log("forceBranch: readback after window lr=%d ud=%d a=%d",
                 slot->lr, slot->ud, slot->a);
        return;
    }
    if (battle_frame < start || battle_frame >= start + len) return;
    if (battle_frame == start)
        sfe::log("forceBranch: window %ld..%ld player %ld word 0x%04lX "
                 "(was lr=%d ud=%d)", start, start + len - 1, player, word,
                 slot->lr, slot->ud);
    forceOne(obj, static_cast<unsigned short>(word));
}

// Which battle frame to scan on. Must be well before the branch frame, and
// after the battle has actually begun.
static long branchScanFrame() {
    const char* v = getenv("SFE_SCAN_FRAME");
    return v && *v ? strtol(v, nullptr, 10) : -1;
}

static void scanForNeedle() {
    const char* hex = getenv("SFE_FIND_HEX");
    if (!hex || !*hex) return;

    // A fixed buffer, not std::vector: `dll/include/sfe/config.hpp` records
    // that any use of the C++ standard library here drags in msvcp140, whose
    // Wine builtin aborts the process at module load. The build guard catches
    // it, which is how this comment came to exist.
    unsigned char needle[MAX_NEEDLE];
    size_t nlen = 0;
    for (const char* q = hex; q[0] && q[1] && nlen < MAX_NEEDLE; q += 2) {
        int hi = hexNybble(q[0]), lo = hexNybble(q[1]);
        if (hi < 0 || lo < 0) break;
        needle[nlen++] = static_cast<unsigned char>((hi << 4) | lo);
    }
    if (nlen < 8) {
        sfe::log("scanForNeedle: needle too short (%u bytes)",
                 static_cast<unsigned>(nlen));
        return;
    }
    sfe::log("scanForNeedle: searching for %u bytes",
             static_cast<unsigned>(nlen));

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto* addr = reinterpret_cast<unsigned char*>(si.lpMinimumApplicationAddress);
    auto* end  = reinterpret_cast<unsigned char*>(si.lpMaximumApplicationAddress);
    int hits = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < end && VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                             | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & readable)
            && !(mbi.Protect & PAGE_GUARD)) {
            auto* base = static_cast<unsigned char*>(mbi.BaseAddress);
            const size_t n = mbi.RegionSize;
            if (n >= nlen) {
                for (size_t i = 0; i + nlen <= n; ++i) {
                    if (base[i] == needle[0]
                        && memcmp(base + i, needle, nlen) == 0) {
                        sfe::log("scanForNeedle: HIT at %p "
                                 "(region %p size %zu protect 0x%lx)",
                                 base + i, mbi.BaseAddress, n, mbi.Protect);
                        // Only a UNIQUE hit identifies the buffer. Two hits
                        // mean the needle is ambiguous and patching either one
                        // would be a guess, so the branch is refused below.
                        if (hits == 0) s_replay_inputs = base + i;
                        if (++hits >= 16) {
                            sfe::log("scanForNeedle: stopping at 16 hits");
                            return;
                        }
                    }
                }
            }
        }
        auto* next = static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    sfe::log("scanForNeedle: done, %d hit(s)", hits);
    if (hits != 1) {
        s_replay_inputs = nullptr;
        sfe::log("scanForNeedle: not unique, refusing to branch");
    }
}

// -------------------------------------------------------------------------
// Mirror scan -- where does this tick's input word actually live?
// -------------------------------------------------------------------------
// Writing char+0x754 does land, but by the time the swap hook can reach it the
// character has already consumed it -- measured: forced input appears in the
// captured p1_input column while p1_x stays bit-identical to the clean run.
// Injection therefore needs a source the engine reads LATER in its own tick.
// SokuLib names one: KeymapManager::inKeys, the field it documents as what
// replays and netplay "copy to/from".
//
// Its offset cannot be read off the header with confidence. KeyInput is
// declared with ten ints but annotated `int[8] (32) 0x38` two lines above, so
// inKeys sits at either +0x58 or +0x60, and there is more than one
// KeymapManager besides. Guessing between them is how the 0x754 attempt burned
// a day: the write landed at a plausible address and meant nothing.
//
// So measure it. This is scanForNeedle's method made temporal: every battle
// frame, keep only the addresses whose uint16 still equals that player's packed
// input word. A wrong address survives one frame by luck and one frame in
// 65536 by coincidence; after a hundred frames of varying input, what survives
// is a mirror of the input word and nothing else. Writable regions only --
// inKeys is written every tick, so anything in .text or .rdata is noise.
constexpr int MIRROR_MAX = 65536;

static DWORD s_mirror[2][MIRROR_MAX];
static int   s_mirror_n[2]    = { -1, -1 };   // -1 = not seeded yet
static int   s_mirror_seen[2] = { 0, 0 };
static int   s_mirror_diff    = 0;   // frames where the two words differed
static long  s_mirror_wait    = 0;   // frames spent waiting for a good seed
static bool  s_mirror_done    = false;

static int popcount16(unsigned short v) {
    int n = 0;
    while (v) { v &= static_cast<unsigned short>(v - 1); ++n; }
    return n;
}

// A candidate's region can be freed between frames, so every follow-up read is
// guarded. SEH rather than a VirtualQuery per address: same safety, and it
// does not cost a syscall per candidate per frame.
static bool mirrorRead(DWORD a, unsigned short* out) {
    __try {
        *out = *reinterpret_cast<volatile unsigned short*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Returns the true match count, which may exceed MIRROR_MAX; the caller needs
// to know it was truncated, because a truncated seed can drop the real answer.
static int mirrorSeed(int who, unsigned short word) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto* addr = reinterpret_cast<unsigned char*>(si.lpMinimumApplicationAddress);
    auto* end  = reinterpret_cast<unsigned char*>(si.lpMaximumApplicationAddress);
    int n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < end && VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
                             | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & writable)
            && !(mbi.Protect & PAGE_GUARD)) {
            auto* base = static_cast<unsigned char*>(mbi.BaseAddress);
            for (size_t i = 0; i + 2 <= mbi.RegionSize; i += 2) {
                if (*reinterpret_cast<unsigned short*>(base + i) == word) {
                    if (n < MIRROR_MAX)
                        s_mirror[who][n] = reinterpret_cast<DWORD>(base + i);
                    ++n;
                }
            }
        }
        auto* next = static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    return n;
}

static void mirrorFilter(int who, unsigned short word) {
    int w = 0;
    for (int i = 0; i < s_mirror_n[who]; ++i) {
        unsigned short v = 0;
        if (mirrorRead(s_mirror[who][i], &v) && v == word)
            s_mirror[who][w++] = s_mirror[who][i];
    }
    s_mirror_n[who] = w;
}

// Relate a surviving address back to a structure the game names, so the answer
// is reusable next run rather than a bare address that means nothing once the
// heap moves.
static void mirrorAnnotate(DWORD a, void* obj, char* out, size_t cap) {
    struct { DWORD base; const char* name; } known[] = {
        { ADDR_INPUT_CLUSTER, "inputMgrCluster" },
        { ADDR_INPUT_MANAGER, "inputMgr"        },
        { ADDR_BATTLE_MANAGER, "&battleMgrPtr"  },
    };
    for (int i = 0; i < 3; ++i) {
        if (a >= known[i].base && a < known[i].base + 0x1000) {
            snprintf(out, cap, "  [%s+0x%lX]", known[i].name, a - known[i].base);
            return;
        }
    }
    const DWORD o = reinterpret_cast<DWORD>(obj);
    if (obj && a >= o && a < o + 0x1000) {
        snprintf(out, cap, "  [char+0x%lX]", a - o);
        return;
    }
    // The KeymapManager a character reads through is reachable by pointer:
    // char+0x750 -> KeyManager -> KeymapManager. Report the offset into
    // whichever of those two it turns out to land in.
    if (obj) {
        __try {
            auto* km = *reinterpret_cast<void**>(
                           reinterpret_cast<char*>(obj) + 0x750);
            if (km) {
                const DWORD k = reinterpret_cast<DWORD>(km);
                if (a >= k && a < k + 0x1000) {
                    snprintf(out, cap, "  [char->keyManager+0x%lX]", a - k);
                    return;
                }
                auto* kmm = *reinterpret_cast<void**>(km);
                const DWORD m = reinterpret_cast<DWORD>(kmm);
                if (kmm && a >= m && a < m + 0x1000) {
                    snprintf(out, cap, "  [char->keymapManager+0x%lX]", a - m);
                    return;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    out[0] = '\0';
}

static void mirrorReport(int who, void* obj) {
    sfe::log("mirrorScan: p%d -- %d survivor(s) after %d frames",
             who + 1, s_mirror_n[who], s_mirror_seen[who]);
    for (int i = 0; i < s_mirror_n[who] && i < 40; ++i) {
        char note[128];
        mirrorAnnotate(s_mirror[who][i], obj, note, sizeof(note));
        sfe::log("  p%d mirror 0x%08lX%s", who + 1, s_mirror[who][i], note);
    }
}

static void mirrorScan(unsigned short p1, unsigned short p2,
                       void* p1obj, void* p2obj) {
    if (s_mirror_done || !getenv("SFE_MIRROR_SCAN")) return;
    const char* fv   = getenv("SFE_MIRROR_FRAMES");
    const int   want = fv && *fv ? static_cast<int>(strtol(fv, nullptr, 10)) : 120;
    const char* bv   = getenv("SFE_MIRROR_MIN_BITS");
    const int   minb = bv && *bv ? static_cast<int>(strtol(bv, nullptr, 10)) : 4;

    const unsigned short w[2] = { p1, p2 };

    if (s_mirror_n[0] < 0 || s_mirror_n[1] < 0) {
        // Seed both players on ONE frame, and only on a frame that can tell
        // them apart. The first run seeded each player independently on its
        // own first nonzero word and came back with two survivors for p2 and
        // none for p1 -- uninterpretable, for two separate reasons this guard
        // fixes.
        //
        // Identical words make each player's candidate set a superset of the
        // other's, so a survivor cannot be attributed to a player.
        //
        // And a low-popcount word is a terrible seed: 0x0004 (left, alone) is
        // one of the commonest 16-bit values in a 32-bit process's memory and
        // matched 94422 addresses, past the cap -- which silently discards
        // candidates, and the discarded set is where the answer was.
        // Seed each player on the first frame its OWN word is distinctive
        // enough, rather than on a frame that suits both. Seeding jointly on a
        // 2-bit word cost the p1 list outright: 0x0006 matched 120714
        // addresses, past the cap, so p1's four "survivors" came from a
        // truncated seed and cannot be trusted, while p2 seeded on 0x0089
        // (3422 candidates) and is sound. Popcount is the lever -- a word with
        // more bits set is a rarer bit pattern in a 32-bit process's memory.
        //
        // Relaxing is what keeps this from stalling: a player who never
        // presses three keys at once would otherwise never seed at all, which
        // is the failure the first 3-bit run produced (no output whatsoever).
        ++s_mirror_wait;
        const char* rv = getenv("SFE_MIRROR_RELAX");
        const long relax = rv && *rv ? strtol(rv, nullptr, 10) : 900;
        int need = minb - static_cast<int>(s_mirror_wait / relax);
        if (need < 1) need = 1;
        for (int k = 0; k < 2; ++k) {
            if (s_mirror_n[k] >= 0) continue;
            if (popcount16(w[k]) < need) continue;
            if (w[k] == w[1 - k]) continue;   // cannot attribute a shared word
            const int n = mirrorSeed(k, w[k]);
            s_mirror_n[k] = n < MIRROR_MAX ? n : MIRROR_MAX;
            s_mirror_seen[k] = 1;
            sfe::log("mirrorScan: p%d seeded on 0x%04X (%d bits) -- %d candidate(s)%s",
                     k + 1, w[k], popcount16(w[k]), n,
                     n > MIRROR_MAX ? "  *** TRUNCATED, answer may be lost ***"
                                    : "");
        }
        return;
    }

    for (int k = 0; k < 2; ++k) {
        mirrorFilter(k, w[k]);
        ++s_mirror_seen[k];
    }
    // How many frames could actually have told the two players apart. Without
    // this the survivor count means nothing: if the two players pressed the
    // same word all run, every one of p1's mirrors survives p2's filter too.
    if (p1 != p2) ++s_mirror_diff;

    if (s_mirror_seen[0] % 20 == 0)
        sfe::log("mirrorScan: %d/%d candidate(s) after %d frames (%d discriminating)",
                 s_mirror_n[0], s_mirror_n[1], s_mirror_seen[0], s_mirror_diff);

    if (s_mirror_seen[0] < want) return;
    sfe::log("mirrorScan: %d of %d frames had p1 != p2",
             s_mirror_diff, s_mirror_seen[0]);
    mirrorReport(0, p1obj);
    mirrorReport(1, p2obj);
    s_mirror_done = true;
}

// Dump the InputManager struct, so a hit can be related back to a field the
// game itself uses rather than to a bare address that means nothing next run.
static void dumpInputManager() {
    if (!getenv("SFE_DUMP_IM")) return;
    auto* im = reinterpret_cast<volatile unsigned char*>(ADDR_INPUT_MANAGER);
    char line[160];
    for (int off = 0; off < 0x180; off += 16) {
        int k = snprintf(line, sizeof(line), "IM+%03X:", off);
        for (int j = 0; j < 16; ++j)
            k += snprintf(line + k, sizeof(line) - k, " %02X", im[off + j]);
        sfe::log("%s", line);
    }
}

// The hook.  Runs on the game's main thread from 0x00407F43.
static int __fastcall HookedSceneProcess(void* This, void* edx) {
    if (InterlockedCompareExchange(&s_start_request, 0, 1) == 1) {
        auto readReplay = reinterpret_cast<PFN_readReplay>(ADDR_READ_REPLAY);
        void* mgr = reinterpret_cast<void*>(ADDR_INPUT_MANAGER);

        // Skipping the replay entirely is the control for a specific
        // suspicion: that a match started with SFE_BATTLE_SUBMODE=0 still has
        // its inputs driven by the decoded replay stream. The evidence for it
        // is that the mirror scan returns the same heap addresses in both
        // submodes, and that the KeymapManagers read [0 0 0 0 0 0] while keyMap
        // is nonzero -- something other than the managers is supplying input.
        // If no replay is loaded and the managers become authoritative, that
        // settles it and the self-play vehicle is a cold VS start.
        const bool skip_replay = getenv("SFE_NO_REPLAY") != nullptr;
        if (skip_replay) sfe::log("SFE_NO_REPLAY: starting a battle with no replay loaded");
        if (!skip_replay && !readReplay(mgr, s_replay_path)) {
            // The game itself says this file is not loadable.  Report it as
            // such rather than letting it look like a navigation failure --
            // that distinction was impossible to make with keypresses.
            InterlockedExchange(&s_start_done, -1);
            return s_orig_scene_process(This, edx);
        }

        dumpInputManager();
        scanForNeedle();
        applyBranch();

        guardInputDeviceSelector();

        const unsigned char rep_mode =
            *reinterpret_cast<volatile unsigned char*>(ADDR_INPUT_MANAGER
                                                       + IM_REPLAY_MODE_OFFSET);
        const int main_mode = skip_replay ? 3
                            : (rep_mode == 0) ? 0 : (rep_mode == 7 ? 7 : 3);

        // The .rep is being used for two different jobs here, and they can be
        // separated. readReplay() has already decoded the match setup --
        // characters, decks, stage, weather -- and setBattleMode() starts a
        // battle with it. Only the SUBMODE decides where the per-frame inputs
        // then come from.
        //
        // Under BATTLE_SUBMODE_REPLAY the decoded input stream is authoritative
        // and nothing downstream can be overridden: writing keyMap at onProcess
        // entry is overwritten by the refill, writing it at swap time is a tick
        // late, and writing inKeys is ignored outright (all three measured, 0
        // of 40 forced frames taking effect). Under a PLAYING submode the
        // engine reads its KeymapManagers instead, which is the surface this
        // module can own.
        //
        // So: same setup, different input source. The replay supplies the
        // matchup and no menu is navigated; the players are ours.
        const char* smv = getenv("SFE_BATTLE_SUBMODE");
        const int sub = smv && *smv ? static_cast<int>(strtol(smv, nullptr, 10))
                                    : BATTLE_SUBMODE_REPLAY;
        const char* bmv = getenv("SFE_BATTLE_MODE");
        const int bmode = bmv && *bmv ? static_cast<int>(strtol(bmv, nullptr, 10))
                                      : main_mode;
        sfe::log("setBattleMode(%d, %d)  [replay says mode %d]",
                 bmode, sub, main_mode);
        reinterpret_cast<PFN_setBattleMode>(ADDR_SET_BATTLE_MODE)(bmode, sub);

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


// -------------------------------------------------------------------------
// Owning the poll
// -------------------------------------------------------------------------
// The hardware watchpoint named the writer of keyMap, and disassembling around
// it settled the whole question. The character's input update at 0x0046C8E0
// does this:
//
//     46c8e6  mov 0x750(%esi),%eax   ; char->keyManager, and
//     46c8ee  je  0x46cabb           ;   if null the copy never happens
//     46c8f7  mov (%eax),%edi        ; edi = KeymapManager*
//     46c900  call *%edx             ; edi->vtable[1]()  -- refills input
//     46c92e  mov 0x38(%edi),%ebp    ; read KeymapManager+0x38 (KeyInput)
//     46c931  mov %ebp,0x754(%esi)   ; write char+0x754, and 0x758.. after it
//
// So KeymapManager+0x38 really was the right surface -- every write to it just
// happened to be undone by the poll at 0x46c900, a few instructions before the
// copy. Writing keyMap directly loses for the mirror-image reason: the copy at
// 0x46c92e comes after any hook that runs at onProcess entry.
//
// Between those two instructions there is exactly one seam, and it is the poll
// itself. Hooking vtable[1] and writing KeyInput *after* calling the original
// means the engine's own copy carries the value through -- whatever the
// original poll read from, device or replay stream, is simply overwritten
// before it is used. Nothing downstream has to be fought.
static PFN_sceneProcess s_orig_kmm_poll = nullptr;
static DWORD*           s_kmm_vtbl      = nullptr;
static void*            s_kmm[2]        = { nullptr, nullptr };
static unsigned short   s_kmm_word[2]   = { 0, 0 };
static bool             s_kmm_drive[2]  = { false, false };
static long             s_poll_calls    = 0;

static void writeKeyInput(void* kmm, unsigned short word) {
    auto* in = reinterpret_cast<volatile int*>(
        reinterpret_cast<char*>(kmm) + KMM_INPUT_OFFSET);
    in[0] = (word & INPUT_LEFT) ? -1 : (word & INPUT_RIGHT) ? 1 : 0;
    in[1] = (word & INPUT_UP)   ? -1 : (word & INPUT_DOWN)  ? 1 : 0;
    in[2] = (word & INPUT_A)      ? 1 : 0;
    in[3] = (word & INPUT_B)      ? 1 : 0;
    in[4] = (word & INPUT_C)      ? 1 : 0;
    in[5] = (word & INPUT_D)      ? 1 : 0;
    in[6] = (word & INPUT_CHANGE) ? 1 : 0;
    in[7] = (word & INPUT_SPELL)  ? 1 : 0;
}

static int __fastcall HookedKeymapPoll(void* This, void* edx) {
    const int r = s_orig_kmm_poll(This, edx);
    ++s_poll_calls;
    for (int i = 0; i < 2; ++i)
        if (This == s_kmm[i] && s_kmm_drive[i])
            writeKeyInput(This, s_kmm_word[i]);
    return r;
}

// Both players' managers are separate objects of the same class, so they share
// one vtable and one patch covers both; `This` tells them apart at call time.
// The looping menu-free match (submode 0) and working injection are otherwise
// mutually exclusive: under a PLAYING submode char+0x750 comes back null, and
// 0x46c8ee then skips the entire copy, so no manager drives the character.
//
// The wiring is a single pointer, and replay submode shows what it should be --
// p1 -> 0x008987F0, p2 -> 0x008987F8, the two static KeyManager objects that
// point at the managers at inputMgr+0x08 and +0x70. Restoring it makes the
// character take the same path it takes during a replay, which is the path the
// poll hook already owns.
constexpr DWORD ADDR_KEY_MGR_P1 = ADDR_INPUT_MANAGER + 0xD8;  // 0x008987F0
constexpr DWORD ADDR_KEY_MGR_P2 = ADDR_INPUT_MANAGER + 0xE0;  // 0x008987F8

static void wireKeyManagers(void* p1obj, void* p2obj) {
    if (!getenv("SFE_WIRE_KEYMGR")) return;
    void* o[2] = { p1obj, p2obj };
    const DWORD want[2] = { ADDR_KEY_MGR_P1, ADDR_KEY_MGR_P2 };
    for (int i = 0; i < 2; ++i) {
        if (!o[i]) continue;
        auto* slot = reinterpret_cast<DWORD*>(
            reinterpret_cast<char*>(o[i]) + CHAR_KEYMANAGER_OFFSET);
        if (*slot) continue;                 // already wired; leave it alone
        *slot = want[i];
        sfe::log("wireKeyManagers: p%d char+0x750 was null, set to 0x%08lX",
                 i + 1, want[i]);
    }
}

// -------------------------------------------------------------------------
// The behaviour policy
// -------------------------------------------------------------------------
// This exists to produce training data the world model can learn a CAUSE from,
// which is a different requirement from producing good play.
//
// Measured on a proxy built to reproduce this game's pathologies, across eight
// seeds: a model fit on confounded logs recovers 28% of the true causal effect
// of an action, while the same model fit on unconfounded on-policy logs
// recovers 91%, statistically indistinguishable from explicit counterfactual
// pairs at 105%. Two properties are doing that work, and both are design
// constraints here rather than nice-to-haves.
//
// FIRST: mode transitions are EXOGENOUS. A human's decision to block comes from
// an intent the logs never contain, and that hidden intent is a back-door path
// from action to next state -- it is exactly what caps the human corpus at 28%.
// Here the mode is drawn from a fixed distribution on a schedule that no game
// state touches. Only the DIRECTION a mode resolves to reads the world, and it
// reads position, which every row records. So the behaviour policy conditions
// on nothing that is not logged, which is the back-door criterion.
//
// SECOND: exploration is TEMPORALLY CORRELATED. Per-frame epsilon-greedy looks
// like exploration and is not: a stance that must survive five frames survives
// 0.82^5 = 0.37 of the time, and on the proxy that under-sampled every
// multi-frame mechanic about threefold -- guarding appeared on 0.2% of frames
// against a real rate near 4%. Blocking IS a multi-frame commitment, so a mode
// is held for a sampled run of frames and the run length is the exploration.
//
// Weights favour the interaction that has to be in the data: someone pressing
// forward with an attack while the other holds away. Blocking is not a mode the
// game has -- holding away from an incoming attack IS the block -- so RETREAT
// carries a long duration, long enough to cover an attack's startup and the
// blockstun after it.
enum PolicyMode {
    PM_NEUTRAL, PM_APPROACH, PM_RETREAT, PM_MELEE,
    PM_BULLET, PM_JUMP, PM_CROUCH, PM_DASH, PM_COUNT
};

static const int PM_WEIGHT[PM_COUNT] = { 4, 24, 25, 23, 12, 5, 3, 4 };
static const int PM_MIN[PM_COUNT]    = {  6,  8, 14,  4,  4,  8,  6,  5 };
static const int PM_MAX[PM_COUNT]    = { 24, 30, 50, 14, 12, 22, 18, 14 };

struct PolicyState { int mode; int left; unsigned short extra; };
static PolicyState s_pol[2] = {};
// Pinning a player to one mode is how the blocking path gets tested directly
// rather than waited for: p1 retreating into p2's melee should produce guard
// frames on p1, and if it does not, the fault is the policy's direction sign
// and not a sampling rate.
static int s_force_mode[2] = { -1, -1 };

// Per-player, per-match style, drawn once at init. A single tuned weight vector
// produced a lopsided match -- p1 attacked so constantly it was never in a
// blockable state and guarded on 0.00% of frames while p2 guarded on 2.33% --
// and the fix is not to hand-tune the vector until that stops. Tuning one point
// is how the proxy got rejected twice, and a world model wants blocking seen
// across many contexts rather than at one aggression level. So each player
// draws its own scaling of the base weights, between half and double, and the
// corpus covers turtles, rushdown, and everything between.
static int s_weight[2][PM_COUNT];
static unsigned int s_rng   = 0x9E3779B9u;
static bool         s_selfplay = false;

static unsigned int rnd32() {
    unsigned int x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s_rng = x;
}
static int rndRange(int lo, int hi) {   // inclusive
    return lo + static_cast<int>(rnd32() % static_cast<unsigned>(hi - lo + 1));
}

static void policyPick(int i) {
    int total = 0;
    for (int m = 0; m < PM_COUNT; ++m) total += s_weight[i][m];
    int r = static_cast<int>(rnd32() % static_cast<unsigned>(total));
    int m = 0;
    while (m < PM_COUNT - 1 && r >= s_weight[i][m]) { r -= s_weight[i][m]; ++m; }
    s_pol[i].mode = m;
    s_pol[i].left = rndRange(PM_MIN[m], PM_MAX[m]);
    // Chosen once per mode, not per frame, for the same reason the mode is:
    // a button that changes every frame is not a decision the dynamics can
    // show a consequence for.
    switch (m) {
        case PM_BULLET: s_pol[i].extra = (rnd32() & 1) ? INPUT_B : INPUT_C; break;
        case PM_DASH:   s_pol[i].extra = (rnd32() & 1) ? 1 : 0; break;  // toward/away
        case PM_JUMP:   s_pol[i].extra = static_cast<unsigned short>(rnd32() % 3); break;
        // Attack HEIGHT, which the first collector had no notion of. Human play
        // shows wrongblock on 0.81% of frames and the first self-play corpus on
        // 0.01% -- a hundredfold gap -- because every attack it threw was a mid
        // and every block was a standing block, so the high/low guess that most
        // of this game's defence consists of never came up once.
        case PM_MELEE:  s_pol[i].extra = static_cast<unsigned short>(rnd32() % 4); break;
        // Crouch-blocking, for the same reason from the defending side.
        // Half, to match the half of melee variants that are lows. A defender
        // guessing height uniformly against a uniform mix blocks correctly as
        // often as it can without reading the attacker -- which it must not do,
        // since reacting would put the opponent's animation into the decision
        // and make the stance predictable from state again.
        case PM_RETREAT: s_pol[i].extra = (rnd32() & 1); break;
        case PM_CROUCH: s_pol[i].extra = static_cast<unsigned short>(rnd32() % 3); break;
        default:        s_pol[i].extra = 0; break;
    }
}

// Distance thresholds. Measured, not chosen: eight matches of the purely
// exogenous policy sat at a median separation of 411px with only 20% of frames
// inside 120px, so melee whiffed almost always and pooled guarding came to
// 0.79% against the human corpus's 4-6%. Two players who never meet cannot
// produce a block.
//
// The fix reads distance, and that is deliberately safe: distance is a function
// of p1_x and p2_x, both of which every row records, so a policy conditioning
// on it still satisfies the back-door criterion. What must never happen is
// conditioning on something UNRECORDED -- a hidden intent -- which is exactly
// the confounder that caps the human corpus. The mode schedule stays exogenous;
// only the action a mode resolves to reads the world.
constexpr float MELEE_RANGE = 220.0f;   // roughly what a melee actually reaches
constexpr float DASH_RANGE  = 150.0f;   // past this, walking in is too slow

// `toward` is +1 when the opponent is to the right of this player.
static unsigned short policyWord(int i, int toward, float dist, int elapsed) {
    if (s_force_mode[i] >= 0) {
        s_pol[i].mode = s_force_mode[i];
        if (--s_pol[i].left <= 0) {
            s_pol[i].left = PM_MAX[s_pol[i].mode];
            if (s_pol[i].mode == PM_BULLET)
                s_pol[i].extra = (rnd32() & 1) ? INPUT_B : INPUT_C;
        }
    } else if (--s_pol[i].left <= 0) policyPick(i);
    const unsigned short fwd  = (toward > 0) ? INPUT_RIGHT : INPUT_LEFT;
    const unsigned short back = (toward > 0) ? INPUT_LEFT  : INPUT_RIGHT;
    switch (s_pol[i].mode) {
        // Walking closes 395px of median separation far too slowly to matter,
        // so cover ground with the dash button and save the walk for in-range
        // spacing.
        case PM_APPROACH: return (dist > DASH_RANGE)
                                 ? static_cast<unsigned short>(fwd | INPUT_D)
                                 : fwd;
        // Holding away IS the block, at EVERY range. Suppressing it when far
        // apart was tried and made things strictly worse -- pooled guarding
        // fell from 0.79% to 0.42% -- because most interaction at range is
        // bullets, and a player not holding back simply gets hit by them
        // instead of blocking them.
        case PM_RETREAT:  return s_pol[i].extra
                                 ? static_cast<unsigned short>(back | INPUT_DOWN)
                                 : back;
        // Walk in first. Pressing A at 400px is a whiffed animation, and a
        // whiff neither hits nor gets blocked, so it teaches the dynamics
        // nothing about either.
        // Walk in only when genuinely out of reach. The first collector gated
        // this at 150px against a median separation of 304px, so melee almost
        // never fired at all: 0.0168 of frames had an active hitbox against
        // human play's 0.0591. Whiffs are not the waste that gate assumed --
        // an active hitbox is a real event the dynamics have to represent.
        case PM_MELEE:
            if (dist > MELEE_RANGE) return fwd;
            switch (s_pol[i].extra) {
                case 1:  return static_cast<unsigned short>(INPUT_DOWN | INPUT_A);  // 2A, a low
                case 2:  return static_cast<unsigned short>(fwd | INPUT_B);
                case 3:  return static_cast<unsigned short>(INPUT_DOWN | INPUT_B);  // 2B, a low
                default: return static_cast<unsigned short>(fwd | INPUT_A);
            }
        case PM_BULLET:   return s_pol[i].extra;
        // Rise, then swing. An air attack is this game's overhead, and a
        // standing block does not stop one -- which is half of what makes
        // blocking a decision rather than a reflex.
        case PM_JUMP:
            if (elapsed < 6)
                return static_cast<unsigned short>(
                    INPUT_UP | (s_pol[i].extra == 1 ? fwd
                              : s_pol[i].extra == 2 ? back : 0));
            return static_cast<unsigned short>(
                (s_pol[i].extra == 1 ? fwd : 0) | INPUT_A);
        case PM_CROUCH:   return s_pol[i].extra == 0
                                 ? INPUT_DOWN
                                 : static_cast<unsigned short>(
                                       INPUT_DOWN | (s_pol[i].extra == 1 ? INPUT_A
                                                                         : INPUT_B));
        case PM_DASH:     return static_cast<unsigned short>(
                              (s_pol[i].extra ? fwd : back) | INPUT_D);
        case PM_NEUTRAL:
        default:          return 0;
    }
}

// Both players, once per tick, from the pre-tick hook -- ahead of the poll that
// the injection hook rides on.
static void policyDrive(void* bm) {
    if (!s_selfplay || !bm) return;
    void* p1obj = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(bm) + BM_PLAYER1_OFFSET);
    void* p2obj = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(bm) + BM_PLAYER2_OFFSET);
    if (!p1obj || !p2obj) return;
    const float x1 = sfe::readPlayerState(p1obj).x;
    const float x2 = sfe::readPlayerState(p2obj).x;
    const float d  = x1 > x2 ? x1 - x2 : x2 - x1;
    s_kmm_word[0]  = policyWord(0, x2 >= x1 ?  1 : -1, d,
                                PM_MAX[s_pol[0].mode] - s_pol[0].left);
    s_kmm_word[1]  = policyWord(1, x1 >= x2 ?  1 : -1, d,
                                PM_MAX[s_pol[1].mode] - s_pol[1].left);
    s_kmm_drive[0] = s_kmm_drive[1] = true;
}

static void policyInit() {
    const char* sp = getenv("SFE_SELFPLAY");
    if (!sp || !*sp) return;
    s_selfplay = true;
    const char* sd = getenv("SFE_SEED");
    if (sd && *sd) s_rng = static_cast<unsigned int>(strtoul(sd, nullptr, 10));
    // Mix the replay identity in. One process handles one replay, so without
    // this every match in a run would draw the same styles and the same mode
    // schedule -- different characters playing an identical script, which is
    // far less than N matches of data. FNV-1a over the path: deterministic, so
    // a run is still reproducible from SFE_SEED alone.
    for (const char* q = s_replay_path; *q; ++q) {
        s_rng ^= static_cast<unsigned char>(*q);
        s_rng *= 16777619u;
    }
    if (!s_rng) s_rng = 0x9E3779B9u;   // xorshift is dead at zero
    const char* fm[2] = { getenv("SFE_P1_MODE"), getenv("SFE_P2_MODE") };
    for (int i = 0; i < 2; ++i) {
        for (int m = 0; m < PM_COUNT; ++m) {
            s_weight[i][m] = PM_WEIGHT[m] * (50 + static_cast<int>(rnd32() % 151)) / 100;
            if (s_weight[i][m] < 1) s_weight[i][m] = 1;
        }
        sfe::log("selfplay: p%d style [%d %d %d %d %d %d %d %d]", i + 1,
                 s_weight[i][0], s_weight[i][1], s_weight[i][2], s_weight[i][3],
                 s_weight[i][4], s_weight[i][5], s_weight[i][6], s_weight[i][7]);
        policyPick(i);
        s_pol[i].left = 1 + i;
        if (fm[i] && *fm[i]) {
            s_force_mode[i] = static_cast<int>(strtol(fm[i], nullptr, 10));
            sfe::log("selfplay: p%d pinned to mode %d", i + 1, s_force_mode[i]);
        }
    }
    sfe::log("selfplay: behaviour policy armed, seed %u", s_rng);
}

static bool installPollHook(void* p1obj, void* p2obj) {
    wireKeyManagers(p1obj, p2obj);
    s_kmm[0] = keymapManagerOf(p1obj);
    s_kmm[1] = keymapManagerOf(p2obj);
    if (!s_kmm[0] || !s_kmm[1]) {
        sfe::log("pollHook: no KeymapManager (p1=%p p2=%p) -- char+0x750 is "
                 "null, so 0x0046C8E0 skips the copy entirely and this player "
                 "is not driven from a manager at all", s_kmm[0], s_kmm[1]);
        return false;
    }
    DWORD* v1 = *reinterpret_cast<DWORD**>(s_kmm[0]);
    DWORD* v2 = *reinterpret_cast<DWORD**>(s_kmm[1]);
    if (v1 != v2)
        sfe::log("pollHook: WARNING p1 and p2 managers have different vtables "
                 "(%p vs %p); only p1's is patched", v1, v2);
    DWORD oldProt = 0;
    if (!VirtualProtect(v1, 8 * sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
        sfe::log("pollHook: VirtualProtect on %p failed (GLE=%lu)", v1,
                 GetLastError());
        return false;
    }
    s_kmm_vtbl       = v1;
    s_orig_kmm_poll  = reinterpret_cast<PFN_sceneProcess>(v1[1]);
    v1[1]            = reinterpret_cast<DWORD>(HookedKeymapPoll);
    DWORD tmp = 0;
    VirtualProtect(v1, 8 * sizeof(DWORD), oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    sfe::log("pollHook: installed on vtable %p slot 1 (orig=%p); "
             "p1 kmm=%p p2 kmm=%p", v1,
             reinterpret_cast<void*>(s_orig_kmm_poll), s_kmm[0], s_kmm[1]);
    return true;
}

static void restorePollHook() {
    if (!s_kmm_vtbl || !s_orig_kmm_poll) return;
    DWORD oldProt = 0;
    if (VirtualProtect(s_kmm_vtbl, 8 * sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
        s_kmm_vtbl[1] = reinterpret_cast<DWORD>(s_orig_kmm_poll);
        DWORD tmp = 0;
        VirtualProtect(s_kmm_vtbl, 8 * sizeof(DWORD), oldProt, &tmp);
        sfe::log("pollHook: restored after %ld poll(s)", s_poll_calls);
    }
    s_kmm_vtbl      = nullptr;
    s_orig_kmm_poll = nullptr;
}

// -------------------------------------------------------------------------
// Who writes keyMap?
// -------------------------------------------------------------------------
// Six injection attempts have now failed for six different reasons, and every
// one of them was a guess about where the engine takes its input from:
// keyMap at onProcess entry (refilled after us), keyMap at swap (a tick late),
// inKeys under replay submode (ignored), the KeymapManagers under a PLAYING
// submode (character not wired to them -- char+0x750 is null), the replay
// decode buffer (works, but its address moves with the replay), and a cold VS
// start with no replay at all (still fed from somewhere).
//
// Guessing is the expensive part. The game knows the answer: some instruction
// writes char+0x754 every tick, and asking which one is a single run.
//
// PAGE_GUARD rather than a debug register, because Wine's DR emulation is not
// something to bet the answer on, and the guard fault carries what is needed
// anyway: ExceptionInformation[0] says read (0) or write (1) and [1] carries
// the faulting address, so writes landing in the eight ints at 0x754 can be
// separated from the rest of the traffic on a very hot page. The guard clears
// itself when it fires, so it is re-armed once per tick from the pre-tick
// hook -- which means one writer is caught per frame and the distinct ones
// accumulate over a few hundred frames.
constexpr int  WATCH_MAX_SITES = 16;
static DWORD   s_watch_site[WATCH_MAX_SITES];
static int     s_watch_hits[WATCH_MAX_SITES];
static int     s_watch_n     = 0;
static DWORD   s_watch_lo    = 0;   // char+0x754
static DWORD   s_watch_hi    = 0;   // char+0x774
static void*   s_watch_veh   = nullptr;
static bool    s_watch_on    = false;

static LONG CALLBACK watchHandler(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode == STATUS_SINGLE_STEP) {
        const DWORD eip = ep->ContextRecord->Eip;
        int i = 0;
        for (; i < s_watch_n; ++i)
            if (s_watch_site[i] == eip) { ++s_watch_hits[i]; break; }
        if (i == s_watch_n && s_watch_n < WATCH_MAX_SITES) {
            s_watch_site[s_watch_n] = eip;
            s_watch_hits[s_watch_n] = 1;
            ++s_watch_n;
            sfe::log("watch: keyMap written from EIP 0x%08lX", eip);
        }
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (er->NumberParameters >= 2 && er->ExceptionInformation[0] == 1) {
        const DWORD at = static_cast<DWORD>(er->ExceptionInformation[1]);
        if (at >= s_watch_lo && at < s_watch_hi) {
            const DWORD eip = ep->ContextRecord->Eip;
            int i = 0;
            for (; i < s_watch_n; ++i)
                if (s_watch_site[i] == eip) { ++s_watch_hits[i]; break; }
            if (i == s_watch_n && s_watch_n < WATCH_MAX_SITES) {
                s_watch_site[s_watch_n] = eip;
                s_watch_hits[s_watch_n] = 1;
                ++s_watch_n;
                sfe::log("watch: keyMap+0x%lX written from EIP 0x%08lX",
                         at - s_watch_lo, eip);
            }
        }
    }
    // The guard is already gone -- that is how PAGE_GUARD works -- so the
    // faulting instruction simply reruns and succeeds.
    return EXCEPTION_CONTINUE_EXECUTION;
}

// PAGE_GUARD marks a whole page and clears on the FIRST access of any kind.
// The character struct is hot, so the first access every frame is a read of a
// neighbouring field, the guard is spent before keyMap is written, and the
// filter never matches -- measured: armed cleanly, caught nothing.
//
// A debug register watches exactly four bytes for writes only, which is the
// question being asked. Dr0 holds the address; in Dr7, L0 enables it, RW0=01
// selects write-only, and LEN0=11 selects four bytes.
static bool armHardwareWatch(DWORD addr) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE th = GetCurrentThread();
    if (!GetThreadContext(th, &ctx)) {
        sfe::log("watch: GetThreadContext failed (GLE=%lu)", GetLastError());
        return false;
    }
    ctx.Dr0 = addr;
    ctx.Dr7 = (ctx.Dr7 & ~0xF000Ful) | 0x1ul | (0x1ul << 16) | (0x3ul << 18);
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!SetThreadContext(th, &ctx)) {
        sfe::log("watch: SetThreadContext failed (GLE=%lu)", GetLastError());
        return false;
    }
    sfe::log("watch: hardware write watch armed on 0x%08lX", addr);
    return true;
}

static void watchArm(void* char_obj) {
    if (!getenv("SFE_WATCH_WRITER") || !char_obj) return;
    if (!s_watch_on) {
        s_watch_veh = AddVectoredExceptionHandler(1, watchHandler);
        if (!s_watch_veh) { sfe::log("watch: AddVectoredExceptionHandler failed"); return; }
        s_watch_lo = reinterpret_cast<DWORD>(char_obj) + CHAR_INPUT_OFFSET;
        s_watch_hi = s_watch_lo + sizeof(SWRCHARINPUT);
        s_watch_on = true;
        sfe::log("watch: armed on 0x%08lX..0x%08lX (p1 keyMap)", s_watch_lo, s_watch_hi);
        if (getenv("SFE_WATCH_HW")) armHardwareWatch(s_watch_lo);
    }
    if (getenv("SFE_WATCH_HW")) return;   // armed once, stays armed
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(s_watch_lo), sizeof(SWRCHARINPUT),
                   PAGE_READWRITE | PAGE_GUARD, &old);
}

static void watchReport() {
    if (!s_watch_on) return;
    sfe::log("watch: %d distinct writer(s) of p1 keyMap", s_watch_n);
    for (int i = 0; i < s_watch_n; ++i)
        sfe::log("  EIP 0x%08lX  %d hit(s)", s_watch_site[i], s_watch_hits[i]);
}

// -------------------------------------------------------------------------
// Sweeping the candidates: which mirror is the one the engine reads?
// -------------------------------------------------------------------------
// Writing keyMap at onProcess entry changed nothing -- 7 transient frames out
// of 6002 differed and none of them were in the forced window -- because the
// engine refills keyMap from its own source further inside the tick. Entry is
// too early and the swap hook is too late, so the write has to go to that
// source instead, before the refill reads it. The mirror scan already found
// every address that holds this tick's word; one of them is it.
//
// Rather than reason about which, write each in turn and let the game answer.
// The capture already reads keyMap after the tick, so if a candidate is the
// real source, the input column for those frames comes back as the forced word
// -- measured on the same rows the corpus is built from, with no extra
// machinery to be wrong about.
// Monotonic across the whole capture, unlike the battle frame counter, which
// resets every round -- a window scheduled on that would reopen once a round.
static long s_bm_calls = 0;

constexpr long SWEEP_WIN = 40;   // frames forced per candidate
constexpr long SWEEP_GAP = 20;   // frames of quiet between, to re-settle
constexpr int  SWEEP_MAX = 64;

static long           s_sweep_base     = -1;
static int            s_sweep_active   = -1;   // candidate index, -1 between windows
static int            s_sweep_hit[SWEEP_MAX]  = {};
static int            s_sweep_tot[SWEEP_MAX]  = {};
static bool           s_sweep_reported = false;
static int            s_sweep_player   = 1;
static unsigned short s_sweep_word     = 0x0005;

// A winning address is only useful if it can be found again. It is on the
// heap, so it moves every run; what has to be established is which stable
// thing it sits at a fixed offset from. Print the candidate anchors and let
// the offsets say which one is constant across runs.
static void dumpAnchors(int n) {
    void* bm = *reinterpret_cast<void**>(ADDR_BATTLE_MANAGER);
    if (!bm) return;
    void* obj[2] = {
        *reinterpret_cast<void**>(reinterpret_cast<char*>(bm) + BM_PLAYER1_OFFSET),
        *reinterpret_cast<void**>(reinterpret_cast<char*>(bm) + BM_PLAYER2_OFFSET),
    };
    DWORD anchor[8];
    const char* name[8];
    int na = 0;
    anchor[na] = reinterpret_cast<DWORD>(bm);       name[na++] = "battleMgr";
    for (int p = 0; p < 2; ++p) {
        anchor[na] = reinterpret_cast<DWORD>(obj[p]);
        name[na++] = p == 0 ? "p1obj" : "p2obj";
        __try {
            auto* km = *reinterpret_cast<void**>(
                           reinterpret_cast<char*>(obj[p]) + 0x750);
            anchor[na] = reinterpret_cast<DWORD>(km);
            name[na++] = p == 0 ? "p1->keyMgr" : "p2->keyMgr";
            anchor[na] = reinterpret_cast<DWORD>(*reinterpret_cast<void**>(km));
            name[na++] = p == 0 ? "p1->keymapMgr" : "p2->keymapMgr";
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    for (int i = 0; i < na; ++i)
        sfe::log("  anchor %-14s = 0x%08lX", name[i], anchor[i]);
    const int who = s_sweep_player - 1;
    for (int i = 0; i < n; ++i) {
        char line[240];
        int k = snprintf(line, sizeof(line), "  0x%08lX offsets:",
                         s_mirror[who][i]);
        for (int j = 0; j < na; ++j)
            k += snprintf(line + k, sizeof(line) - k, " %s%+ld",
                          name[j],
                          static_cast<long>(s_mirror[who][i]) -
                          static_cast<long>(anchor[j]));
        sfe::log("%s", line);
    }
}

static void sweepReport(int n) {
    const int who = s_sweep_player - 1;
    sfe::log("sweep: results for p%d, forced word 0x%04X", s_sweep_player,
             s_sweep_word);
    for (int i = 0; i < n; ++i) {
        char note[128];
        mirrorAnnotate(s_mirror[who][i], nullptr, note, sizeof(note));
        sfe::log("  candidate 0x%08lX%s -- keyMap took the forced word on "
                 "%d of %d frames", s_mirror[who][i], note,
                 s_sweep_hit[i], s_sweep_tot[i]);
    }
    dumpAnchors(n);
}

// Called from the pre-tick hook, before the engine refills keyMap.
static void sweepStep() {
    if (!s_mirror_done || !getenv("SFE_SWEEP")) { s_sweep_active = -1; return; }
    const int who = s_sweep_player - 1;
    int n = s_mirror_n[who];
    if (n > SWEEP_MAX) n = SWEEP_MAX;
    if (n <= 0) { s_sweep_active = -1; return; }

    if (s_sweep_base < 0) {
        s_sweep_base = s_bm_calls;
        sfe::log("sweep: %d candidate(s) for p%d, forcing 0x%04X in %ld-frame "
                 "windows", n, s_sweep_player, s_sweep_word, SWEEP_WIN);
    }
    const long t   = s_bm_calls - s_sweep_base;
    const long k   = t / (SWEEP_WIN + SWEEP_GAP);
    const long pos = t % (SWEEP_WIN + SWEEP_GAP);
    if (k >= n) {
        if (!s_sweep_reported) { sweepReport(n); s_sweep_reported = true; }
        s_sweep_active = -1;
        return;
    }
    if (pos >= SWEEP_WIN) { s_sweep_active = -1; return; }

    s_sweep_active = static_cast<int>(k);
    if (pos == 0)
        sfe::log("sweep: candidate %ld = 0x%08lX", k, s_mirror[who][k]);
    __try {
        *reinterpret_cast<volatile unsigned short*>(s_mirror[who][k])
            = s_sweep_word;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Called from the capture, after the tick has run, with the swept player's
// word as the character actually consumed it.
static void sweepObserve(unsigned short w) {
    const int k = s_sweep_active;
    if (k < 0 || k >= SWEEP_MAX) return;
    ++s_sweep_tot[k];
    if (w == s_sweep_word) ++s_sweep_hit[k];
}

// -------------------------------------------------------------------------
// The pre-update injection point
// -------------------------------------------------------------------------
// char+0x754 is what the character logic consumes, and writing it as a
// SWRCHARINPUT does land -- but from the swap hook it lands after the fact.
// Measured: the captured p1_input column showed the forced word while p1_x
// stayed bit-identical to the clean run, because the character had already
// moved on that tick. The write was real and too late.
//
// So it has to happen inside the tick: after the engine has filled keyMap from
// whatever source it is using this run -- replay stream, device, or netplay's
// inKeys -- and before the characters update. SokuLib documents that boundary.
// BattleManager's third virtual is the update that runs
//
//     for (p : players) { ...; p->update(); ...; p->updatePhysics(); }
//
// so entering it is after the fill and before the first p->update(). Hooking
// BattleManager's vtable rather than the scene's also scopes the hook to a
// battle, which is the only time any of these offsets mean anything.
//
// PAGE_READWRITE, not PAGE_WRITECOPY: the latter silently fails on .rdata
// under Wine and leaves the patch unapplied, which is how an earlier attempt
// at this same vtable came back looking like the hook never fired.
constexpr DWORD ADDR_VTBL_BATTLE_MANAGER = 0x008588EC;

static PFN_sceneProcess s_orig_bm_update = nullptr;
static DWORD*           s_bm_vtbl        = nullptr;
static int              s_bm_slot        = 2;
static bool             s_bm_hooked      = false;

// The same eight ints readPlayerInput reads, packed back to a word. A free
// function because the hook is not a Session member and must not need one.
static unsigned short peekKeyMap(void* char_obj) {
    if (!char_obj) return 0;
    auto* i = reinterpret_cast<volatile SWRCHARINPUT*>(
        reinterpret_cast<char*>(char_obj) + CHAR_INPUT_OFFSET);
    unsigned short w = 0;
    if (i->lr < 0) w |= INPUT_LEFT;  else if (i->lr > 0) w |= INPUT_RIGHT;
    if (i->ud < 0) w |= INPUT_UP;    else if (i->ud > 0) w |= INPUT_DOWN;
    if (i->a)  w |= INPUT_A;
    if (i->b)  w |= INPUT_B;
    if (i->c)  w |= INPUT_C;
    if (i->d)  w |= INPUT_D;
    if (i->ch) w |= INPUT_CHANGE;
    if (i->s)  w |= INPUT_SPELL;
    return w;
}

struct InjectCfg {
    bool           armed;
    long           frame, len;
    int            player;   // 1, 2, or 3 for both
    unsigned short word;
};
static InjectCfg s_inject      = { false, 0, 0, 1, 0 };
static bool      s_inject_read = false;

static void injectInit() {
    if (s_inject_read) return;
    s_inject_read = true;
    const char* sp = getenv("SFE_SWEEP_PLAYER");
    const char* sw = getenv("SFE_SWEEP_WORD");
    if (sp && *sp) s_sweep_player = static_cast<int>(strtol(sp, nullptr, 10));
    if (sw && *sw) s_sweep_word   = static_cast<unsigned short>(strtol(sw, nullptr, 0));
    const char* fr = getenv("SFE_INJECT_FRAME");
    if (!fr || !*fr) return;
    const char* le = getenv("SFE_INJECT_LEN");
    const char* wd = getenv("SFE_INJECT_WORD");
    const char* pl = getenv("SFE_INJECT_PLAYER");
    s_inject.frame  = strtol(fr, nullptr, 10);
    s_inject.len    = le && *le ? strtol(le, nullptr, 10) : 60;
    s_inject.word   = static_cast<unsigned short>(
                          wd && *wd ? strtol(wd, nullptr, 0) : 0);
    s_inject.player = pl && *pl ? static_cast<int>(strtol(pl, nullptr, 10)) : 1;
    s_inject.armed  = true;
    sfe::log("inject: player %d word 0x%04X battle frames %ld..%ld",
             s_inject.player, s_inject.word, s_inject.frame,
             s_inject.frame + s_inject.len - 1);
}

static int __fastcall HookedBattleUpdate(void* This, void* edx) {
    injectInit();
    // `This` is the scene now, not the BattleManager, so the frame counter and
    // the two characters come from the manager the same way the capture reads
    // them -- which also keeps the two paths reading one source of truth.
    void* bm = *reinterpret_cast<void**>(ADDR_BATTLE_MANAGER);
    policyDrive(bm);
    sweepStep();
    if (bm) {
        watchArm(*reinterpret_cast<void**>(
            reinterpret_cast<char*>(bm) + BM_PLAYER1_OFFSET));
        if (s_bm_calls == 400) watchReport();
    }
    // Is this slot the per-frame update at all? "VUnknown08 // maybe update"
    // is SokuLib's own confidence level, and a slot called once a round looks
    // identical to a working hook until the frame window never opens -- which
    // is exactly what the first run showed.
    if (++s_bm_calls <= 3 || s_bm_calls % 600 == 0) {
        const long f = bm ? static_cast<long>(
            *reinterpret_cast<volatile uint32_t*>(
                reinterpret_cast<char*>(bm) + BM_FRAME_COUNT_OFFSET)) : -1;
        sfe::log("preTick: call %ld, scene=%p bm=%p frameCount=%ld",
                 s_bm_calls, This, bm, f);
    }
    if (s_inject.armed && bm) {
        const long f = static_cast<long>(
            *reinterpret_cast<volatile uint32_t*>(
                reinterpret_cast<char*>(bm) + BM_FRAME_COUNT_OFFSET));
        if (f >= s_inject.frame && f < s_inject.frame + s_inject.len) {
            for (int p = 1; p <= 2; ++p) {
                if (s_inject.player != 3 && s_inject.player != p) continue;
                void* obj = *reinterpret_cast<void**>(
                    reinterpret_cast<char*>(bm)
                    + (p == 1 ? BM_PLAYER1_OFFSET : BM_PLAYER2_OFFSET));
                if (!obj) continue;
                // Log the boundary frames only. What the engine had just put
                // there is the interesting half: if keyMap is already the
                // forced word on the frame after the window opens, something
                // upstream is echoing us rather than the hook doing the work.
                if (f == s_inject.frame || f == s_inject.frame + 1) {
                    sfe::log("inject: frame %ld p%d keyMap was 0x%04X, forcing 0x%04X",
                             f, p, peekKeyMap(obj), s_inject.word);
                    if (p == 1) {
                        void* o2 = *reinterpret_cast<void**>(
                            reinterpret_cast<char*>(bm) + BM_PLAYER2_OFFSET);
                        dumpKeymapManagers(obj, o2);
                    }
                }
                // Arm the poll hook rather than writing anything here: the
                // value has to land after 0x46c900 and before 0x46c92e, and
                // this hook is outside that window on both sides.
                s_kmm_drive[p - 1] = true;
                s_kmm_word[p - 1]  = s_inject.word;
                const char* via = getenv("SFE_INJECT_VIA");
                if (via && strcmp(via, "old") == 0) {
                    forceViaKeymap(obj, p, s_inject.word);
                    forceOne(obj, s_inject.word);
                }
            }
        } else if (f >= s_inject.frame + s_inject.len) {
            s_kmm_drive[0] = s_kmm_drive[1] = false;
        }
        if (f == s_inject.frame + s_inject.len) {
            void* obj = *reinterpret_cast<void**>(
                reinterpret_cast<char*>(bm)
                + (s_inject.player == 2 ? BM_PLAYER2_OFFSET : BM_PLAYER1_OFFSET));
            sfe::log("inject: frame %ld window closed, keyMap now 0x%04X",
                     f, peekKeyMap(obj));
        }
    }
    return s_orig_bm_update(This, edx);
}

static bool installPreTickHook() {
    // Which vtable, decided by looking rather than by naming a constant.
    // SokuLib has four battle scene classes -- Battle, BattleServer,
    // BattleClient, BattleWatch -- with four different vtables, and replay
    // playback does not obviously use the first. Reading the pointer off the
    // live scene object gets the right one without having to know which, and
    // logging it says which it turned out to be.
    //
    // Slot 1 is IScene::onProcess: `virtual int onProcess()`, no arguments.
    // That matters for more than correctness. A blind slot sweep is not
    // available here -- a __thiscall virtual that takes arguments expects the
    // callee to clean them (`ret N`), while this hook's __fastcall thunk
    // returns with `ret`, so hooking a slot with a different signature
    // unbalances the stack and crashes. onProcess is the one slot whose
    // signature is documented and argument-free.
    void* scene = currentSceneObject();
    if (!scene) return false;
    DWORD* vtbl = *reinterpret_cast<DWORD**>(scene);
    if (!vtbl) {
        sfe::log("ERROR: battle scene %p has a null vtable", scene);
        return false;
    }
    const char* sv = getenv("SFE_HOOK_SLOT");
    s_bm_slot = sv && *sv ? static_cast<int>(strtol(sv, nullptr, 10))
                          : VTBL_SCENE_ONPROCESS;

    DWORD oldProt = 0;
    if (!VirtualProtect(vtbl, 8 * sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
        sfe::log("ERROR: VirtualProtect on battle scene vtable %p failed (GLE=%lu)",
                 vtbl, GetLastError());
        return false;
    }
    s_bm_vtbl        = vtbl;
    s_orig_bm_update = reinterpret_cast<PFN_sceneProcess>(vtbl[s_bm_slot]);
    vtbl[s_bm_slot]  = reinterpret_cast<DWORD>(HookedBattleUpdate);

    DWORD tmp = 0;
    VirtualProtect(vtbl, 8 * sizeof(DWORD), oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

    const char* which = vtbl == reinterpret_cast<DWORD*>(0x008574A0) ? "Battle"
                      : vtbl == reinterpret_cast<DWORD*>(0x0085758C) ? "BattleWatch"
                      : vtbl == reinterpret_cast<DWORD*>(0x00857518) ? "BattleServer"
                      : vtbl == reinterpret_cast<DWORD*>(0x00857570) ? "BattleClient"
                      : "unrecognised";
    sfe::log("Pre-tick hook installed: scene=%p vtbl=%p (%s) slot=%d orig=%p",
             scene, vtbl, which, s_bm_slot,
             reinterpret_cast<void*>(s_orig_bm_update));
    return true;
}

static void restorePreTickHook() {
    if (!s_bm_vtbl || !s_orig_bm_update) return;
    DWORD oldProt = 0;
    if (VirtualProtect(s_bm_vtbl, 8 * sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
        s_bm_vtbl[s_bm_slot] = reinterpret_cast<DWORD>(s_orig_bm_update);
        DWORD tmp = 0;
        VirtualProtect(s_bm_vtbl, 8 * sizeof(DWORD), oldProt, &tmp);
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        sfe::log("Pre-tick hook restored");
    }
    s_bm_vtbl        = nullptr;
    s_orig_bm_update = nullptr;
    s_bm_hooked      = false;
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
        restorePreTickHook();
        restorePollHook();
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
            sfe::log("Pre-tick hook restored");
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
        const char* mfv = getenv("SFE_MAX_FRAMES");
        const int max_frames = mfv && *mfv
            ? static_cast<int>(strtol(mfv, nullptr, 10)) : MAX_FRAMES_PER_REPLAY;
        if (m_frame_index >= max_frames) {
            sfe::log("Hit frame cap (%d)", max_frames);
            finish(AutoState::DRAINING, nullptr);
            break;
        }

        // Read both players' inputs and state for the tick being presented.
        uint16_t p1 = 0, p2 = 0;
        uint32_t battle_frame = 0;
        sfe::PlayerState p1s, p2s;
        if (void* bm = *reinterpret_cast<void**>(ADDR_BATTLE_MANAGER)) {
            // Lazily, because the BattleManager does not exist until a battle
            // does -- and its vtable is what carries the pre-update hook.
            if (!s_bm_hooked) {
                s_bm_hooked = installPreTickHook();
                policyInit();
                installPollHook(
                    *reinterpret_cast<void**>(
                        reinterpret_cast<char*>(bm) + BM_PLAYER1_OFFSET),
                    *reinterpret_cast<void**>(
                        reinterpret_cast<char*>(bm) + BM_PLAYER2_OFFSET));
            }
            // The engine's own battle tick. Recorded because `m_frame_index`
            // cannot identify a moment in the match: it is this capture's row
            // number and starts at 0 wherever the capture armed, which is not
            // the same tick every run. Two captures of replay 5314129 -- one
            // by the collection fleet, one here -- turned out to be offset by
            // exactly one, discoverable only by cross-correlating the input
            // columns. With this column that alignment is read off, not
            // inferred.
            battle_frame = *reinterpret_cast<const uint32_t*>(
                               reinterpret_cast<char*>(bm) + BM_FRAME_COUNT_OFFSET);
            void* p1obj = *reinterpret_cast<void**>(
                              reinterpret_cast<char*>(bm) + BM_PLAYER1_OFFSET);
            void* p2obj = *reinterpret_cast<void**>(
                              reinterpret_cast<char*>(bm) + BM_PLAYER2_OFFSET);
            // FORCE, then read, so the CSV records what was written and the
            // read-back on the following frame says whether it survived. The
            // ordering question -- does this hook run between the replay's
            // input fill and the character logic, or after both -- is settled
            // by the state diverging or not, which is cheaper to test than to
            // reason about from the frame loop.
            forceBranchInput(p1obj, p2obj, static_cast<long>(battle_frame));
            p1 = readPlayerInput(p1obj);
            p2 = readPlayerInput(p2obj);
            // Same two objects, same tick, one more read each. Position and
            // guard state are what the world model cannot learn from pixels --
            // see sfe/player_state.hpp for the measurements that led here.
            p1s = sfe::readPlayerState(p1obj);
            p2s = sfe::readPlayerState(p2obj);
            // Bullets, in a second pass because each player's are ranked by
            // how close they are to the OTHER player -- the one being shot at
            // -- and that position is only known once both states are read.
            sfe::readProjectiles(p1obj, p2s.x, p2s.y, &p1s);
            sfe::readProjectiles(p2obj, p1s.x, p1s.y, &p2s);
            // Where does this tick's word live? Answered from the same two
            // objects, on frames the game is already being read on.
            mirrorScan(p1, p2, p1obj, p2obj);
            sweepObserve(s_sweep_player == 2 ? p2 : p1);
        }

        tag.capture      = true;
        // SCAN AND PATCH DURING PLAYBACK, NOT AT LOAD.
        //
        // The buffer readReplay() produces is a transient decode scratch --
        // patching it changed nothing, because the game re-reads the inputs
        // from elsewhere once the battle starts. Measured: the hit sat at
        // offset 0xFD934 of a 1 MB PAGE_READWRITE region, and the branched run
        // was byte-identical to the clean one. So do it here, on a battle
        // frame, when the buffer the game is actually consuming exists.
        if (m_frame_index == branchScanFrame()) {
            scanForNeedle();
            applyBranch();
        }
        tag.frame_index  = m_frame_index;
        tag.battle_frame = battle_frame;
        tag.p1_input    = p1;
        tag.p2_input    = p2;
        tag.p1_state    = p1s;
        tag.p2_state    = p2s;
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
