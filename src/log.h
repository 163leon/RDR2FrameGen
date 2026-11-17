#pragma once

namespace fg {

// Opens (truncates) the log file. Safe to call once from DllMain: no threads, no loader work.
void LogOpen(const wchar_t* path);

// printf-style line with a timestamp and thread id. Thread safe; a no-op before LogOpen.
void Log(const char* format, ...);

// Same, but only written when the config asks for detailed logging.
void LogDebug(const char* format, ...);

void SetDebugLogging(bool enabled);

}  // namespace fg
