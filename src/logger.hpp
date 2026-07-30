#pragma once
// =========================================================================
// SokuFrameExtractor — logger.hpp
// =========================================================================
// File-based logger. NO console, NO stdout redirection. Safe to call from
// any thread at any time after initLog() succeeds. Designed specifically
// to avoid the Wine + SWRSToys loader-lock issues that AllocConsole and
// freopen(CONOUT$) trigger.
// =========================================================================

namespace sfe {

/// Open (or create/truncate) the log file. Returns true if the file could
/// be opened. Safe to call from DllMain callbacks — only a fopen happens.
bool initLog(const char* path);

/// Flush and close the log file. Call from DLL_PROCESS_DETACH.
void closeLog();

/// Printf-style logging. Timestamp + newline are added automatically.
/// Each call flushes so the log survives crashes. Thread-safe.
void log(const char* fmt, ...);

/// Log a formatted hex dump of `n` DWORDs at `addr`. Useful for verifying
/// vtable patches pre/post write.
void logDwords(const char* prefix, const void* addr, int n);

} // namespace sfe