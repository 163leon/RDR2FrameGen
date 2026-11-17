// RDR2FrameGen: DLSS Frame Generation for Red Dead Redemption 2 on Vulkan.
// Loaded as an .asi by the dinput8 ASI loader, before the game creates its Vulkan instance.

#include <windows.h>

#include <string>

#include "camera.h"
#include "config.h"
#include "crash_log.h"
#include "log.h"
#include "version.h"
#include "vk_route.h"

namespace {

std::wstring ModuleDirectory(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring directory(path);
    const size_t slash = directory.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : directory.substr(0, slash);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH) {
        // reserved != null: the process is exiting in an orderly way (ExitProcess), not being killed.
        fg::Log("Unloading (%s)", reserved ? "process exit" : "FreeLibrary");
        if (!reserved && fg::Cfg().enabled) fg::CameraUnregisterScript(module);
        return TRUE;
    }
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);

    const std::wstring directory = ModuleDirectory(module);
    fg::LogOpen((directory + L"\\RDR2FrameGen.log").c_str());
    fg::LoadConfig((directory + L"\\RDR2FrameGen.ini").c_str());
    fg::SetDebugLogging(fg::Cfg().debugLogging);

    fg::Log("RDR2FrameGen %s loaded; enabled=%d debugLogging=%d toggleKey=0x%X", FG_VERSION, fg::Cfg().enabled,
            fg::Cfg().debugLogging, fg::Cfg().toggleKey);
    // Installed even when the mod is disabled: a crash report from a run without frame generation
    // tells whether a crash is ours or comes from the game or another mod.
    fg::InstallCrashLog();
    if (!fg::Cfg().enabled) {
        fg::Log("Disabled in RDR2FrameGen.ini; only the crash report stays active, the game is otherwise untouched.");
        return TRUE;
    }
    fg::InstallVulkanRoute(directory);
    fg::CameraRegisterScript(module);
    return TRUE;
}
