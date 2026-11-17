#pragma once

namespace fg {

// Logs fatal exceptions (access violations and the like) anywhere in the process, then lets them
// continue to the game's own handlers unchanged.
void InstallCrashLog();

// Diagnostics for hangs: logs the call stack of every other thread in the process.
void LogAllThreadStacks(const char* reason);

}  // namespace fg
