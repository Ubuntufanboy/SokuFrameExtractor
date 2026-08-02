// =========================================================================
// SokuFrameExtractor — logger.cpp
// =========================================================================

#include "sfe/logger.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace sfe {

namespace {
    FILE* s_file = nullptr;

    // CRITICAL_SECTION rather than std::mutex: <mutex> pulls in msvcp140,
    // whose Wine builtin aborts the process (see config.hpp). Initialised
    // lazily on first use -- the logger is the very first thing Initialize()
    // touches, so it cannot depend on anything else having run.
    CRITICAL_SECTION s_lock;
    bool             s_lock_ready = false;

    void ensureLock() {
        if (!s_lock_ready) {
            InitializeCriticalSection(&s_lock);
            s_lock_ready = true;
        }
    }

    struct Guard {
        Guard()  { ensureLock(); EnterCriticalSection(&s_lock); }
        ~Guard() { LeaveCriticalSection(&s_lock); }
    };
}

bool initLog(const char* path) {
    Guard lk;
    if (s_file) {
        fclose(s_file);
        s_file = nullptr;
    }
    // "w" truncates on each run so the log file always reflects the
    // current session. Use "a" instead if you want to keep history.
    s_file = fopen(path, "w");
    return s_file != nullptr;
}

void closeLog() {
    Guard lk;
    if (s_file) {
        fflush(s_file);
        fclose(s_file);
        s_file = nullptr;
    }
}

void log(const char* fmt, ...) {
    Guard lk;
    if (!s_file) return;

    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_s(&tm_buf, &now);
    fprintf(s_file, "[%02d:%02d:%02d] ",
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    va_list args;
    va_start(args, fmt);
    vfprintf(s_file, fmt, args);
    va_end(args);

    fprintf(s_file, "\n");
    fflush(s_file); // Flush every message so we survive a crash.
}

void logDwords(const char* prefix, const void* addr, int n) {
    Guard lk;
    if (!s_file) return;

    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_s(&tm_buf, &now);
    fprintf(s_file, "[%02d:%02d:%02d] %s @ %p:",
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, prefix, addr);

    const unsigned int* p = static_cast<const unsigned int*>(addr);
    for (int i = 0; i < n; ++i) {
        fprintf(s_file, " %08X", p[i]);
    }
    fprintf(s_file, "\n");
    fflush(s_file);
}

} // namespace sfe