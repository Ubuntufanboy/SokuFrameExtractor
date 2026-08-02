// =========================================================================
// SokuFrameExtractor — main.cpp
// =========================================================================
// DLL entry point for the SWRSToys module system.
//
// IMPORTANT: SWRSToys calls Initialize() from inside its own DllMain, which
// means Initialize() runs under the Windows loader lock.  Keep it to opening a
// log, parsing the .ini, and handing off to Session::init().  No console, no
// stdout redirection, no process creation.
// =========================================================================

#include <winsock2.h>
#include <windows.h>

#include "sfe/config.hpp"
#include "sfe/session.hpp"
#include "sfe/logger.hpp"

#include <shlwapi.h>
#include <cstdio>
#include <cstring>

static HMODULE s_hModule = nullptr;
static char    s_iniPath[MAX_PATH + 256] = {};
static char    s_logPath[MAX_PATH + 256] = {};

// =========================================================================
// Configuration loading
// =========================================================================
// Every key read here has a matching entry in config/sfe.ini and a matching
// field in sfe::Config.  tests/test_config_parity.py enforces that, because
// the three had silently diverged: the .ini advertised SaveAsBMP,
// EncoderThreads and UseRenderTarget (read by nothing), while the loader read
// FifoPath and UseVAAPI (present in no .ini), and SkipFrames/UseVAAPI were
// parsed and logged but never used by any code path.
static sfe::Config loadConfig() {
    auto readStr = [](const char* key, const char* def, char* out) {
        GetPrivateProfileStringA("General", key, def, out, sfe::SFE_PATH_MAX,
                                 s_iniPath);
    };
    auto readBool = [](const char* key, bool def) -> bool {
        return GetPrivateProfileIntA("General", key, def ? 1 : 0, s_iniPath) != 0;
    };

    sfe::Config cfg;
    readStr ("ReplayDir",   "replay",                  cfg.replay_dir);
    readStr ("OutputDir",   "soku_extract",            cfg.output_dir);
    readStr ("FifoPath",    "Z:\\tmp\\sfe_video.fifo",  cfg.fifo_path);
    readStr ("StatusPath",  "Z:\\tmp\\sfe_status.json", cfg.status_path);
    cfg.fast_forward = readBool("FastForward", true);
    cfg.verbose      = readBool("Verbose",     false);
    return cfg;
}

// =========================================================================
// SWRSToys Module Exports
// =========================================================================

// -------------------------------------------------------------------------
// Game version guard
// -------------------------------------------------------------------------
// ADDR_BATTLE_MANAGER, VTBL_CBATTLEMANAGER and ADDR_FRAME_DELAY in session.cpp
// are absolute addresses into the loaded game image. Against a different build
// they do not fail gracefully: Initialize() writes a function pointer into
// whatever occupies 0x008588EC and the game corrupts itself. The previous
// version returned true unconditionally ("Accept any game version"), turning a
// detectable mismatch into an undebuggable crash.
//
// The check runs against the RUNNING MODULE rather than a file on disk,
// because the two are not the same thing here:
//
//   th123e.exe  SizeOfImage 0x00007000  -- a 28 KB launcher. Running it is
//               what selects the English translation (th123e.dll), which is
//               what this project collects on.
//   th123.exe   SizeOfImage 0x004A4000  -- the actual 4.8 MB game, and the
//               image our addresses point into (0x008985E4 sits inside
//               ImageBase 0x400000 + 0x4A4000).
//
// So hashing the executable that was launched would validate the launcher,
// not the code we patch. Reading the PE header of the module actually mapped
// at 0x400000 validates exactly the thing the addresses depend on, and it does
// not care which stub started it.
struct GameVersion {
    DWORD time_date_stamp;
    DWORD size_of_image;
    const char* name;
};

static const GameVersion k_supported[] = {
    { 0x4E72D68B, 0x004A4000, "Hisoutensoku v1.10a (th123.exe)" },
};

// Returns the matching entry, or nullptr if the loaded image is unknown.
static const GameVersion* identifyGameImage() {
    auto base = reinterpret_cast<const BYTE*>(GetModuleHandleA(nullptr));
    if (!base) return nullptr;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    for (const auto& v : k_supported) {
        if (nt->FileHeader.TimeDateStamp == v.time_date_stamp &&
            nt->OptionalHeader.SizeOfImage == v.size_of_image) {
            return &v;
        }
    }
    return nullptr;
}

// SWRSToys calls this before Initialize with an MD5 it computes itself. Which
// file that digest covers is not specified by the loader's public interface,
// and it demonstrably does not match either executable on disk here, so it is
// not a signal we can gate on. Accept, and do the real check in Initialize()
// where a log file exists and the game image is actually mapped.
extern "C" __declspec(dllexport) bool CheckVersion(const BYTE /*hash*/[16]) {
    return true;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule,
                                                 HMODULE hParentModule) {
    s_hModule = hMyModule;

    // ----- Step 1: file logging, next to the DLL -------------------------
    GetModuleFileNameA(hMyModule, s_logPath, sizeof(s_logPath));
    PathRemoveFileSpecA(s_logPath);
    PathAppendA(s_logPath, "SokuFrameExtractor.log");

    if (!sfe::initLog(s_logPath))
        sfe::initLog("SokuFrameExtractor.log"); // last resort: CWD

    sfe::log("=========================================================");
    sfe::log("  SokuFrameExtractor — Initialize()");
    sfe::log("=========================================================");
    sfe::log("DLL module    : %p", static_cast<void*>(hMyModule));
    sfe::log("Parent module : %p", static_cast<void*>(hParentModule));

    // ----- Step 1b: refuse to patch an image we do not recognise ---------
    if (const GameVersion* v = identifyGameImage()) {
        sfe::log("Game image    : %s", v->name);
    } else {
        auto base = reinterpret_cast<const BYTE*>(GetModuleHandleA(nullptr));
        DWORD tds = 0, soi = 0;
        if (base) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                                     base + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE) {
                    tds = nt->FileHeader.TimeDateStamp;
                    soi = nt->OptionalHeader.SizeOfImage;
                }
            }
        }
        sfe::log("ERROR: unrecognised game image "
                 "(TimeDateStamp=0x%08lX SizeOfImage=0x%08lX)", tds, soi);
        sfe::log("This module hardcodes addresses for Hisoutensoku v1.10a. "
                 "Refusing to patch — see k_supported in dll/src/main.cpp.");
        return false;
    }

    // ----- Step 2: locate the .ini ---------------------------------------
    GetModuleFileNameA(hMyModule, s_iniPath, sizeof(s_iniPath));
    PathRemoveFileSpecA(s_iniPath);
    PathAppendA(s_iniPath, "SokuFrameExtractor.ini");
    sfe::log("Config file   : %s", s_iniPath);

    if (GetFileAttributesA(s_iniPath) == INVALID_FILE_ATTRIBUTES)
        sfe::log("WARNING: .ini not found — using defaults");

    // ----- Step 3: load config -------------------------------------------
    sfe::Config cfg;
    try {
        cfg = loadConfig();
    } catch (const std::exception& e) {
        sfe::log("EXCEPTION loading config: %s", e.what());
        return false;
    }

    sfe::log("Config:");
    sfe::log("  ReplayDir   = %s", cfg.replay_dir);
    sfe::log("  OutputDir   = %s", cfg.output_dir);
    sfe::log("  FifoPath    = %s", cfg.fifo_path);
    sfe::log("  StatusPath  = %s", cfg.status_path);
    sfe::log("  FastForward = %d", cfg.fast_forward);
    sfe::log("  Verbose     = %d", cfg.verbose);

    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    sfe::log("CWD: %s", cwd);

    char replayAbs[MAX_PATH];
    GetFullPathNameA(cfg.replay_dir, sizeof(replayAbs), replayAbs, nullptr);
    sfe::log("Replay dir (abs): %s", replayAbs);

    const DWORD attrs = GetFileAttributesA(replayAbs);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        sfe::log("ERROR: replay dir missing or not a directory: %s", replayAbs);
        return false;
    }

    // ----- Step 4: start the session -------------------------------------
    bool ok = false;
    try {
        ok = sfe::getSession().init(cfg);
    } catch (const std::exception& e) {
        sfe::log("EXCEPTION in Session::init: %s", e.what());
        return false;
    } catch (...) {
        sfe::log("UNKNOWN EXCEPTION in Session::init");
        return false;
    }

    if (!ok) {
        sfe::log("Session::init returned false");
        return false;
    }

    sfe::log("Initialize() succeeded");
    return true;
}

// =========================================================================
// DLL Entry Point
// =========================================================================

extern "C" int APIENTRY DllMain(HMODULE hModule,
                                DWORD   fdwReason,
                                LPVOID  /*lpReserved*/) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        s_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        break;

    case DLL_PROCESS_DETACH:
        // Normal termination goes through Session::writeStatusAndExit, which
        // calls ExitProcess after flushing. This path covers the abnormal case
        // (host unloading us) and must still restore the game's vtable.
        sfe::log("DLL_PROCESS_DETACH");
        sfe::getSession().shutdown();
        sfe::closeLog();
        break;
    }
    return TRUE;
}
