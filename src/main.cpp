// =========================================================================
// SokuFrameExtractor — main.cpp
// =========================================================================
// DLL entry point for the SWRSToys module system.
//
// IMPORTANT: SWRSToys calls Initialize() from inside its own DllMain (see
// swrstoys/dummy.cpp::DllMain -> Hook() -> LoadLibrary + Initialize). That
// means Initialize() runs under the Windows loader lock.
//
// This file does only the minimum required in Initialize(): open a log
// file, parse the .ini, call FrameExtractor::init(). No console, no
// stdout redirection, no extra process creation.
// =========================================================================

#include <winsock2.h>
#include <windows.h>

#include "config.hpp"
#include "frame_extractor.hpp"
#include "logger.hpp"

#include <shlwapi.h>
#include <cstdio>
#include <cstring>

static HMODULE s_hModule = nullptr;
static char    s_iniPath[MAX_PATH + 256] = {};
static char    s_logPath[MAX_PATH + 256] = {};

// =========================================================================
// Configuration loading
// =========================================================================

static sfe::Config loadConfig() {
    sfe::Config cfg;

    auto readStr = [](const char* section, const char* key,
                      const char* def, const char* ini) -> std::string {
        char buf[512];
        GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), ini);
        return buf;
    };

    auto readInt = [](const char* section, const char* key,
                      int def, const char* ini) -> int {
        return GetPrivateProfileIntA(section, key, def, ini);
    };

    cfg.output_dir   = readStr("General", "OutputDir",   "soku_extract",            s_iniPath);
    cfg.replay_dir   = readStr("General", "ReplayDir",   "replay",                  s_iniPath);
    cfg.fifo_path    = readStr("General", "FifoPath",    "Z:\\tmp\\sfe_video.fifo", s_iniPath);
    cfg.fast_forward = readInt("General", "FastForward", 1,  s_iniPath) != 0;
    cfg.verbose      = readInt("General", "Verbose",     0,  s_iniPath) != 0;
    cfg.skip_frames  = readInt("General", "SkipFrames",  0,  s_iniPath);
    cfg.use_vaapi    = readInt("General", "UseVAAPI",    1,  s_iniPath) != 0;

    return cfg;
}

// =========================================================================
// SWRSToys Module Exports
// =========================================================================

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE /*hash*/[16]) {
    // Accept any game version.
    return true;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule,
                                                  HMODULE hParentModule) {
    s_hModule = hMyModule;

    // ----- Step 1: Set up file logging (next to the DLL) -----------------
    GetModuleFileNameA(hMyModule, s_logPath, sizeof(s_logPath));
    PathRemoveFileSpecA(s_logPath);
    PathAppendA(s_logPath, "SokuFrameExtractor.log");

    if (!sfe::initLog(s_logPath))
        sfe::initLog("SokuFrameExtractor.log"); // last-ditch: CWD

    sfe::log("=========================================================");
    sfe::log("  SokuFrameExtractor - Initialize() called");
    sfe::log("=========================================================");
    sfe::log("DLL module handle  : %p", static_cast<void*>(hMyModule));
    sfe::log("Parent module      : %p", static_cast<void*>(hParentModule));
    sfe::log("Log file path      : %s", s_logPath);

    // ----- Step 2: Build the .ini path -----------------------------------
    GetModuleFileNameA(hMyModule, s_iniPath, sizeof(s_iniPath));
    PathRemoveFileSpecA(s_iniPath);
    PathAppendA(s_iniPath, "SokuFrameExtractor.ini");
    sfe::log("Config file path   : %s", s_iniPath);

    if (GetFileAttributesA(s_iniPath) == INVALID_FILE_ATTRIBUTES)
        sfe::log("WARNING: INI file not found, using defaults");

    // ----- Step 3: Load configuration ------------------------------------
    sfe::Config cfg;
    try {
        cfg = loadConfig();
    } catch (const std::exception& e) {
        sfe::log("EXCEPTION loading config: %s", e.what());
        return false;
    }

    sfe::log("Config:");
    sfe::log("  OutputDir    = %s", cfg.output_dir.c_str());
    sfe::log("  ReplayDir    = %s", cfg.replay_dir.c_str());
    sfe::log("  FifoPath     = %s", cfg.fifo_path.c_str());
    sfe::log("  FastForward  = %d", cfg.fast_forward);
    sfe::log("  Verbose      = %d", cfg.verbose);
    sfe::log("  SkipFrames   = %d", cfg.skip_frames);
    sfe::log("  UseVAAPI     = %d", cfg.use_vaapi);

    // ----- Step 4: CWD + absolute-path diagnostics -----------------------
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    sfe::log("CWD: %s", cwd);

    char replayAbs[MAX_PATH], outputAbs[MAX_PATH];
    GetFullPathNameA(cfg.replay_dir.c_str(), sizeof(replayAbs), replayAbs, nullptr);
    GetFullPathNameA(cfg.output_dir.c_str(), sizeof(outputAbs), outputAbs, nullptr);
    sfe::log("Replay dir (abs) : %s", replayAbs);
    sfe::log("Output dir (abs) : %s", outputAbs);

    DWORD replayAttrs = GetFileAttributesA(replayAbs);
    if (replayAttrs == INVALID_FILE_ATTRIBUTES) {
        sfe::log("ERROR: Replay directory does not exist: %s", replayAbs);
        return false;
    }
    if (!(replayAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        sfe::log("ERROR: Replay path exists but is not a directory");
        return false;
    }

    // ----- Step 5: Initialize the frame extractor ------------------------
    sfe::log("Calling FrameExtractor::init()...");

    bool initOk = false;
    try {
        initOk = sfe::getExtractor().init(cfg);
    } catch (const std::exception& e) {
        sfe::log("EXCEPTION in FrameExtractor::init: %s", e.what());
        return false;
    } catch (...) {
        sfe::log("UNKNOWN EXCEPTION in FrameExtractor::init");
        return false;
    }

    if (!initOk) {
        sfe::log("FrameExtractor::init() returned false");
        return false;
    }

    sfe::log("Initialize() succeeded!");
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
        sfe::log("DLL_PROCESS_DETACH - shutting down");
        sfe::getExtractor().shutdown();
        sfe::closeLog();
        break;
    }
    return TRUE;
}