// =========================================================================
// SokuFrameExtractor — logger.cpp
// =========================================================================

#include "logger.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace sfe {

namespace {
    FILE*      s_file = nullptr;
    std::mutex s_mutex;
}

bool initLog(const char* path) {
    std::lock_guard<std::mutex> lk(s_mutex);
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
    std::lock_guard<std::mutex> lk(s_mutex);
    if (s_file) {
        fflush(s_file);
        fclose(s_file);
        s_file = nullptr;
    }
}

void log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(s_mutex);
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
    std::lock_guard<std::mutex> lk(s_mutex);
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