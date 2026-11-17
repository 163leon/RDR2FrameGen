// The few ScriptHookRDR2 entry points this mod calls.
//
// ScriptHookRDR2.dll (by Alexander Blade) runs mod scripts on the game's script thread and lets them
// call the game's native functions. Its SDK is not redistributable, so the exported functions used
// here are declared by hand and the import library is generated from scripthook.def at build time.

#pragma once

#include <windows.h>

#include <cstring>

__declspec(dllimport) void scriptWait(DWORD milliseconds);
__declspec(dllimport) void scriptRegister(HMODULE module, void (*scriptMain)());
__declspec(dllimport) void scriptUnregister(HMODULE module);
__declspec(dllimport) void nativeInit(UINT64 hash);
__declspec(dllimport) void nativePush64(UINT64 value);
__declspec(dllimport) PUINT64 nativeCall();

namespace fg {

// How the game returns a vector from a native function: each component in its own 8-byte slot.
struct NativeVector3 {
    float x, padX;
    float y, padY;
    float z, padZ;
};

// Calls a game native function by hash. Arguments are passed as 64-bit slots.
template <typename Result, typename... Arguments>
Result CallNative(UINT64 hash, Arguments... arguments) {
    nativeInit(hash);
    const auto push = [](auto argument) {
        static_assert(sizeof(argument) <= sizeof(UINT64));
        UINT64 slot = 0;
        std::memcpy(&slot, &argument, sizeof(argument));
        nativePush64(slot);
    };
    (push(arguments), ...);
    Result result;
    std::memcpy(&result, nativeCall(), sizeof(Result));
    return result;
}

}  // namespace fg
