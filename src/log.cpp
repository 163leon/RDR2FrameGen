#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace fg {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<bool> g_debug{false};
ULONGLONG g_start = 0;

void Write(const char* format, va_list args) {
    if (g_file == INVALID_HANDLE_VALUE) return;

    char line[1024];
    const ULONGLONG ms = GetTickCount64() - g_start;
    int used = snprintf(line, sizeof(line), "[%6llu.%03llu t%05lu] ", ms / 1000, ms % 1000, GetCurrentThreadId());
    if (used < 0) return;
    const int body = vsnprintf(line + used, sizeof(line) - used - 2, format, args);
    if (body < 0) return;
    used += body < static_cast<int>(sizeof(line)) - used - 2 ? body : static_cast<int>(sizeof(line)) - used - 3;
    line[used++] = '\r';
    line[used++] = '\n';

    AcquireSRWLockExclusive(&g_lock);
    DWORD written = 0;
    WriteFile(g_file, line, static_cast<DWORD>(used), &written, nullptr);
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace

void LogOpen(const wchar_t* path) {
    g_start = GetTickCount64();
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    Write(format, args);
    va_end(args);
}

void LogDebug(const char* format, ...) {
    if (!g_debug.load(std::memory_order_relaxed)) return;
    va_list args;
    va_start(args, format);
    Write(format, args);
    va_end(args);
}

void SetDebugLogging(bool enabled) { g_debug.store(enabled, std::memory_order_relaxed); }

}  // namespace fg
