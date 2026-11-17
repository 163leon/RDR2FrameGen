// Diagnostics: writes fatal exceptions to the mod log with module-relative addresses and a short
// call stack. The game exits silently on a crash, so without this there is nothing to go on.

#include "crash_log.h"

#include <windows.h>

#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace fg {
namespace {

std::atomic<int> g_reports{0};
constexpr int kMaxReports = 6;

// "module.dll+0x1234" for an address, or the raw address when it is outside every module.
void Describe(const void* address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module) &&
        module) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        snprintf(out, size, "%ls+0x%llX", name,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    } else {
        snprintf(out, size, "%p", address);
    }
}

// What lives at a faulting data address: memory state, and the module that used to be there if it
// has since been unloaded (a stale pointer into an unloaded DLL is a classic cause).
void DescribeMemory(uintptr_t address) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory))) {
        Log("  memory at 0x%llX: state=0x%lX protect=0x%lX type=0x%lX allocationBase=%p regionSize=0x%llX",
            static_cast<unsigned long long>(address), memory.State, memory.Protect, memory.Type, memory.AllocationBase,
            static_cast<unsigned long long>(memory.RegionSize));
    }

    // ntdll keeps a ring of recently unloaded modules.
    struct UnloadEvent {
        void* base;
        SIZE_T size;
        ULONG sequence;
        ULONG timeDateStamp;
        ULONG checkSum;
        WCHAR name[32];
        ULONG version[2];
    };
    using GetTraceFn = void(NTAPI*)(ULONG** elementSize, ULONG** elementCount, void** trace);
    const auto getTrace = reinterpret_cast<GetTraceFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetUnloadEventTraceEx"));
    if (!getTrace) return;
    ULONG* elementSize = nullptr;
    ULONG* elementCount = nullptr;
    void* trace = nullptr;
    getTrace(&elementSize, &elementCount, &trace);
    if (!elementSize || !elementCount || !trace) return;
    const auto* base = *reinterpret_cast<const unsigned char* const*>(trace);
    if (!base) return;
    for (ULONG index = 0; index < *elementCount; ++index) {
        const auto* event = reinterpret_cast<const UnloadEvent*>(base + static_cast<size_t>(index) * *elementSize);
        if (!event->base) continue;
        const uintptr_t start = reinterpret_cast<uintptr_t>(event->base);
        if (address < start || address >= start + event->size) continue;
        Log("  the address is inside an unloaded module: %.32ls %p size=0x%llX", event->name, event->base,
            static_cast<unsigned long long>(event->size));
    }
}

bool ReadMemory(uintptr_t address, void* out, size_t size) {
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &done) && done == size;
}

bool NameCharacter(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

// The game runs its own scripts in a virtual machine, and a crash inside a script command shows only
// the game's code in the call stack. The running script's name sits inside one of the objects the
// stack points to, so list every identifier-like string (lower case, digits, underscores) found in
// the first bytes of those objects. Most are noise; a script name among them tells whose command it was.
void LogNamesNearStack(uintptr_t rsp) {
    constexpr size_t kStackBytes = 0x1800, kObjectBytes = 0x800, kMaxLines = 80;
    static unsigned long long stack[kStackBytes / 8];
    static unsigned char object[kObjectBytes];
    static unsigned seen[kMaxLines];
    size_t lines = 0;
    // The stack may end sooner than the window (the main thread's frames sit near its top): read it
    // page by page and treat what cannot be read as empty.
    memset(stack, 0, sizeof(stack));
    size_t readable = 0;
    for (size_t offset = 0; offset < sizeof(stack); offset += 0x100) {
        if (!ReadMemory(rsp + offset, reinterpret_cast<unsigned char*>(stack) + offset, 0x100)) break;
        readable = offset + 0x100;
    }
    Log("  stack bytes readable from rsp: 0x%zX", readable);
    if (!readable) return;
    Log("  identifier-like strings in objects referenced from the stack (looking for the running script):");
    for (size_t slot = 0; slot < kStackBytes / 8 && lines < kMaxLines; ++slot) {
        const uintptr_t pointer = static_cast<uintptr_t>(stack[slot]);
        if (pointer < 0x10000 || pointer > 0x7FFFFFFFFFFFull || (pointer & 7)) continue;
        size_t available = kObjectBytes;
        if (!ReadMemory(pointer, object, available)) {
            available = 0x100;
            if (!ReadMemory(pointer, object, available)) continue;
        }
        for (size_t start = 0; start + 8 < available && lines < kMaxLines; ++start) {
            if (start && NameCharacter(object[start - 1])) continue;
            size_t length = 0;
            bool letters = false, underscore = false;
            while (start + length < available && NameCharacter(object[start + length])) {
                letters = letters || (object[start + length] >= 'a' && object[start + length] <= 'z');
                underscore = underscore || object[start + length] == '_';
                ++length;
            }
            if (length < 6 || length > 63 || start + length >= available || object[start + length] != 0 || !letters || !underscore) {
                start += length;
                continue;
            }
            unsigned hash = 2166136261u;
            for (size_t i = 0; i < length; ++i) hash = (hash ^ object[start + i]) * 16777619u;
            bool known = false;
            for (size_t i = 0; i < lines; ++i) known = known || seen[i] == hash;
            if (!known) {
                seen[lines++] = hash;
                Log("    stack+0x%zX -> object+0x%zX: %.*s", slot * 8, start, static_cast<int>(length), object + start);
            }
            start += length;
        }
    }
}

bool IsFatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case 0xC0000409:  // stack buffer overrun / fast fail
        case 0xC0000374:  // heap corruption
            return true;
        default:
            return false;
    }
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (!IsFatal(record->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_reports.fetch_add(1) >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;

    char where[320];
    Describe(record->ExceptionAddress, where, sizeof(where));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        Log("EXCEPTION 0x%08lX at %s: %s address 0x%llX", record->ExceptionCode, where,
            record->ExceptionInformation[0] == 1 ? "write to" : record->ExceptionInformation[0] == 8 ? "execute at" : "read of",
            static_cast<unsigned long long>(record->ExceptionInformation[1]));
    } else {
        Log("EXCEPTION 0x%08lX at %s", record->ExceptionCode, where);
    }

    const CONTEXT& c = *info->ContextRecord;
    Log("  rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX rbp=%llX rsp=%llX", c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp);
    Log("  r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX r13=%llX r14=%llX r15=%llX", c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
    if (c.Rip && !IsBadReadPtr(reinterpret_cast<const void*>(c.Rip - 16), 32)) {
        const auto* code = reinterpret_cast<const unsigned char*>(c.Rip - 16);
        char bytes[32 * 3 + 1];
        for (int i = 0; i < 32; ++i) snprintf(bytes + i * 3, 4, "%02X ", code[i]);
        Log("  code at rip-16: %s", bytes);
    }
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        DescribeMemory(static_cast<uintptr_t>(record->ExceptionInformation[1]));
    }

    // Walk the stack from the faulting context (a copy: the unwinder modifies it).
    CONTEXT context = *info->ContextRecord;
    for (int frame = 0; frame < 20; ++frame) {
        Describe(reinterpret_cast<const void*>(context.Rip), where, sizeof(where));
        Log("  #%02d %s", frame, where);

        // A call through a null pointer leaves Rip = 0 with the caller's return address on the stack.
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = context.Rip ? RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr) : nullptr;
        if (function) {
            void* handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                             &establisherFrame, nullptr);
        } else {
            // Leaf function: the return address is at the top of the stack.
            if (IsBadReadPtr(reinterpret_cast<const void*>(context.Rsp), sizeof(DWORD64))) break;
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
        }
        if (!context.Rip) break;
    }
    LogNamesNearStack(static_cast<uintptr_t>(info->ContextRecord->Rsp));
    return EXCEPTION_CONTINUE_SEARCH;
}

// Unwinds a thread context into return addresses. No logging here: other threads may be suspended.
int Unwind(CONTEXT context, DWORD64* out, int max) {
    int count = 0;
    for (; count < max && context.Rip; ++count) {
        out[count] = context.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (function) {
            void* handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
        } else {
            DWORD64 next = 0;
            if (!ReadMemory(static_cast<uintptr_t>(context.Rsp), &next, sizeof(next))) break;
            context.Rip = next;
            context.Rsp += sizeof(DWORD64);
        }
    }
    return count;
}

}  // namespace

void InstallCrashLog() { AddVectoredExceptionHandler(1, &OnException); }

void LogAllThreadStacks(const char* reason) {
    constexpr int kMaxThreads = 256, kMaxFrames = 24;
    static DWORD ids[kMaxThreads];
    static DWORD64 frames[kMaxThreads][kMaxFrames];
    static int depth[kMaxThreads];
    int threads = 0;

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL more = Thread32First(snapshot, &entry); more && threads < kMaxThreads; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                ids[threads] = entry.th32ThreadID;
                depth[threads] = Unwind(context, frames[threads], kMaxFrames);
                ++threads;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);

    Log("Thread stacks (%s): %d threads", reason, threads);
    char where[160];
    for (int t = 0; t < threads; ++t) {
        char line[2048];
        int used = snprintf(line, sizeof(line), "  thread %lu:", ids[t]);
        for (int f = 0; f < depth[t] && used < static_cast<int>(sizeof(line)) - 170; ++f) {
            Describe(reinterpret_cast<const void*>(frames[t][f]), where, sizeof(where));
            used += snprintf(line + used, sizeof(line) - used, " %s", where);
        }
        Log("%s", line);
    }
}

}  // namespace fg
