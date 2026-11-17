#include "sl_runtime.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <iterator>

#include "config.h"
#include "log.h"

namespace fg {
namespace {

// NGX application id the game's own DLSS integration is registered under.
constexpr uint32_t kApplicationId = 0x60E8AB7;

HMODULE g_interposer = nullptr;
std::wstring g_runtimeDirectory;  // "<game>\RDR2FrameGen\", lower case, with trailing slash

void OnStreamlineLog(sl::LogType type, const char* message) {
    if (!message) return;
    // Streamline ends its lines with a newline; ours adds one.
    size_t length = strlen(message);
    while (length && (message[length - 1] == '\n' || message[length - 1] == '\r')) --length;
    if (type == sl::LogType::eError) {
        Log("[SL error] %.*s", static_cast<int>(length), message);
    } else if (type == sl::LogType::eWarn) {
        Log("[SL warn] %.*s", static_cast<int>(length), message);
    } else {
        LogDebug("[SL] %.*s", static_cast<int>(length), message);
    }
}

std::wstring Lower(std::wstring text) {
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

void LogRequirements(sl::Feature feature, const char* name) {
    sl::FeatureRequirements requirements{};
    const sl::Result result = slGetFeatureRequirements(feature, requirements);
    if (result != sl::Result::eOk) {
        Log("%s: slGetFeatureRequirements failed, result=%d", name, static_cast<int>(result));
        return;
    }
    Log("%s requirements: flags=0x%X queues graphics=%u compute=%u opticalFlow=%u, extensions instance=%u device=%u",
        name, static_cast<unsigned>(requirements.flags), requirements.vkNumGraphicsQueuesRequired,
        requirements.vkNumComputeQueuesRequired, requirements.vkNumOpticalFlowQueuesRequired,
        requirements.vkNumInstanceExtensions, requirements.vkNumDeviceExtensions);
}

}  // namespace

bool SlStart(const std::wstring& gameDirectory) {
    // Static: Streamline is handed pointers into this string.
    static std::wstring runtime;
    runtime = gameDirectory + L"\\RDR2FrameGen";
    g_runtimeDirectory = Lower(runtime + L"\\");

    const std::wstring interposerPath = runtime + L"\\sl.interposer.dll";
    g_interposer = LoadLibraryExW(interposerPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_interposer) {
        Log("Streamline not loaded: %ls missing or failed to load (Win32 error %lu)", interposerPath.c_str(),
            GetLastError());
        return false;
    }

    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    static const wchar_t* pluginPaths[1];
    pluginPaths[0] = runtime.c_str();

    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = Cfg().debugLogging ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = runtime.c_str();
    preferences.logMessageCallback = &OnStreamlineLog;
    // Manual hooking: we route only the Vulkan calls Streamline must see. No over-the-air downloads.
    preferences.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
    preferences.applicationId = kApplicationId;
    preferences.engine = sl::EngineType::eCustom;
    preferences.renderAPI = sl::RenderAPI::eVulkan;

    const sl::Result result = slInit(preferences);
    if (result != sl::Result::eOk) {
        Log("slInit failed, result=%d; frame generation is unavailable", static_cast<int>(result));
        return false;
    }
    Log("Streamline initialised from %ls", runtime.c_str());

    LogRequirements(sl::kFeatureDLSS_G, "DLSS-G");
    LogRequirements(sl::kFeatureReflex, "Reflex");
    LogRequirements(sl::kFeaturePCL, "PCL");
    return true;
}

HMODULE SlInterposer() { return g_interposer; }

bool IsStreamlineAddress(const void* address) {
    if (g_runtimeDirectory.empty()) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module) ||
        !module) {
        return false;
    }

    // Callers come from a handful of modules; remember the last answer.
    static std::atomic<HMODULE> lastModule{nullptr};
    static std::atomic<bool> lastAnswer{false};
    if (lastModule.load(std::memory_order_acquire) == module) return lastAnswer.load(std::memory_order_acquire);

    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    const std::wstring lower = Lower(std::wstring(path, length));
    const bool inside = lower.compare(0, g_runtimeDirectory.size(), g_runtimeDirectory) == 0;
    lastAnswer.store(inside, std::memory_order_release);
    lastModule.store(module, std::memory_order_release);
    return inside;
}

bool SlReportDevice(void* vkPhysicalDevice) {
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = vkPhysicalDevice;
    const sl::Result dlssg = slIsFeatureSupported(sl::kFeatureDLSS_G, adapter);
    const sl::Result reflex = slIsFeatureSupported(sl::kFeatureReflex, adapter);
    Log("Feature support on the game's device: DLSS-G result=%d, Reflex result=%d (0 = supported)",
        static_cast<int>(dlssg), static_cast<int>(reflex));
    return dlssg == sl::Result::eOk && reflex == sl::Result::eOk;
}

}  // namespace fg
