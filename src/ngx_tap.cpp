#include "ngx_tap.h"

#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_vk.h>
#include <nvsdk_ngx_params.h>

#include <MinHook.h>

#include <intrin.h>

#include <mutex>
#include <string>

#include "frames.h"
#include "log.h"
#include "sl_runtime.h"

namespace fg {
namespace {

using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using CreateFeature1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using GetParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
// Init entry points differ in their argument lists between SDK versions; all arguments are integers
// or pointers, so ten register/stack slots passed through unchanged cover every variant.
using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(uint64_t);

// Two NGX runtimes live in the process: _nvngx.dll (loaded by Streamline) and the legacy nvngx.dll
// that the game's older NGX SDK loads. The game's DLSS goes through the second one.
constexpr int kRuntimes = 2;
const wchar_t* const kRuntimeNames[kRuntimes] = {L"_nvngx.dll", L"nvngx.dll"};
constexpr int kInitVariants = 5;
const char* const kInitNames[kInitVariants] = {"NVSDK_NGX_VULKAN_Init", "NVSDK_NGX_VULKAN_Init_Ext", "NVSDK_NGX_VULKAN_Init_Ext2",
                                               "NVSDK_NGX_VULKAN_Init_ProjectID", "NVSDK_NGX_VULKAN_Init_ProjectID_Ext"};

struct Runtime {
    bool installed{};
    CreateFeatureFn createFeature{};
    CreateFeature1Fn createFeature1{};
    EvaluateFeatureFn evaluateFeature{};
    ReleaseFeatureFn releaseFeature{};
    GetParametersFn getCapabilityParameters{};
    InitFn init[kInitVariants]{};
    ShutdownFn shutdown{};
    ShutdownFn shutdown1{};
};
Runtime g_runtimes[kRuntimes];

// One runtime may forward to the other; only the outermost call on a thread is observed.
thread_local int t_depth = 0;
struct DepthGuard {
    DepthGuard() { ++t_depth; }
    ~DepthGuard() { --t_depth; }
    bool outermost() const { return t_depth == 1; }
};

const char* CallerName(void* returnAddress) {
    return IsStreamlineAddress(returnAddress) ? "Streamline" : "game";
}

// The game's DLSS upscaler instances and the flags they were created with.
struct Upscaler {
    const NVSDK_NGX_Handle* handle{};
    int flags{};
};
std::mutex g_upscalersMutex;
Upscaler g_upscalers[8];

void RememberUpscaler(const NVSDK_NGX_Handle* handle, int flags) {
    std::lock_guard<std::mutex> lock(g_upscalersMutex);
    for (Upscaler& slot : g_upscalers) {
        if (!slot.handle || slot.handle == handle) {
            slot = {handle, flags};
            return;
        }
    }
}

void ForgetUpscaler(const NVSDK_NGX_Handle* handle) {
    std::lock_guard<std::mutex> lock(g_upscalersMutex);
    for (Upscaler& slot : g_upscalers) {
        if (slot.handle == handle) slot = {};
    }
}

bool FindUpscaler(const NVSDK_NGX_Handle* handle, int& flags) {
    std::lock_guard<std::mutex> lock(g_upscalersMutex);
    for (const Upscaler& slot : g_upscalers) {
        if (slot.handle == handle) {
            flags = slot.flags;
            return true;
        }
    }
    return false;
}

void OnFeatureCreated(NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle, NVSDK_NGX_Result result) {
    if (feature != NVSDK_NGX_Feature_SuperSampling || !NVSDK_NGX_SUCCEED(result) || !handle || !*handle || !parameters) return;
    int flags = 0;
    unsigned int width = 0, height = 0, outWidth = 0, outHeight = 0;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
    parameters->Get(NVSDK_NGX_Parameter_Width, &width);
    parameters->Get(NVSDK_NGX_Parameter_Height, &height);
    parameters->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth);
    parameters->Get(NVSDK_NGX_Parameter_OutHeight, &outHeight);
    RememberUpscaler(*handle, flags);
    Log("Game created its DLSS upscaler: %ux%u -> %ux%u, flags=0x%X (hdr=%d mvLowRes=%d mvJittered=%d depthInverted=%d)", width,
        height, outWidth, outHeight, flags, (flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0,
        (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0, (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0,
        (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0);
}

bool ReadImage(const NVSDK_NGX_Parameter* parameters, const char* name, VkImage& image, VkImageView& view, VkFormat& format, uint32_t& width, uint32_t& height) {
    void* pointer = nullptr;
    if (!NVSDK_NGX_SUCCEED(parameters->Get(name, &pointer)) || !pointer) return false;
    const auto* resource = static_cast<const NVSDK_NGX_Resource_VK*>(pointer);
    if (resource->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return false;
    const NVSDK_NGX_ImageViewInfo_VK& info = resource->Resource.ImageViewInfo;
    if (!info.Image || !info.ImageView || info.Format == VK_FORMAT_UNDEFINED || !info.Width || !info.Height) return false;
    image = info.Image;
    view = info.ImageView;
    format = info.Format;
    width = info.Width;
    height = info.Height;
    return true;
}

// Returns false when the parameters do not describe a usable DLSS upscaling pass.
bool ReadInputs(VkCommandBuffer cmd, const NVSDK_NGX_Parameter* parameters, int flags, DlssInputs& in) {
    in = DlssInputs{};
    in.commandBuffer = cmd;
    if (!ReadImage(parameters, NVSDK_NGX_Parameter_Depth, in.depth, in.depthView, in.depthFormat, in.depthWidth, in.depthHeight)) return false;
    if (!ReadImage(parameters, NVSDK_NGX_Parameter_MotionVectors, in.motion, in.motionView, in.motionFormat, in.motionWidth, in.motionHeight)) return false;

    unsigned int renderWidth = 0, renderHeight = 0, reset = 0;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &renderWidth);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &renderHeight);
    in.renderWidth = renderWidth && renderWidth <= in.depthWidth ? renderWidth : in.depthWidth;
    in.renderHeight = renderHeight && renderHeight <= in.depthHeight ? renderHeight : in.depthHeight;

    if (!NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &in.jitterX)) ||
        !NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &in.jitterY)) ||
        !NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &in.motionScaleX)) ||
        !NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &in.motionScaleY))) {
        return false;
    }
    if (in.motionScaleX == 0.0f || in.motionScaleY == 0.0f) return false;
    parameters->Get(NVSDK_NGX_Parameter_Reset, &reset);

    in.reset = reset != 0;
    in.depthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    in.motionJittered = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
    in.motionLowRes = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    return true;
}

template <int R>
NVSDK_NGX_Result NVSDK_CONV Hook_CreateFeature(VkCommandBuffer cmd, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
    DepthGuard guard;
    void* const caller = _ReturnAddress();
    const NVSDK_NGX_Result result = g_runtimes[R].createFeature(cmd, feature, parameters, handle);
    if (guard.outermost()) {
        if (!IsStreamlineAddress(caller)) Log("Game NGX CreateFeature(feature=%d) via %ls -> 0x%X", static_cast<int>(feature), kRuntimeNames[R], static_cast<unsigned>(result));
        OnFeatureCreated(feature, parameters, handle, result);
    }
    return result;
}

template <int R>
NVSDK_NGX_Result NVSDK_CONV Hook_CreateFeature1(VkDevice device, VkCommandBuffer cmd, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
    DepthGuard guard;
    void* const caller = _ReturnAddress();
    const NVSDK_NGX_Result result = g_runtimes[R].createFeature1(device, cmd, feature, parameters, handle);
    if (guard.outermost()) {
        if (!IsStreamlineAddress(caller)) Log("Game NGX CreateFeature1(feature=%d) via %ls -> 0x%X", static_cast<int>(feature), kRuntimeNames[R], static_cast<unsigned>(result));
        OnFeatureCreated(feature, parameters, handle, result);
    }
    return result;
}

template <int R>
NVSDK_NGX_Result NVSDK_CONV Hook_ReleaseFeature(NVSDK_NGX_Handle* handle) {
    ForgetUpscaler(handle);
    return g_runtimes[R].releaseFeature(handle);
}

template <int R>
NVSDK_NGX_Result NVSDK_CONV Hook_EvaluateFeature(VkCommandBuffer cmd, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters, void* callback) {
    DepthGuard guard;
    int flags = 0;
    // Streamline evaluates its own NGX features (frame generation itself) through the same entry
    // point; only the upscaler instances the game created are of interest.
    if (guard.outermost() && cmd && parameters && FindUpscaler(handle, flags)) {
        DlssInputs inputs;
        if (ReadInputs(cmd, parameters, flags, inputs)) {
            FrameOnDlssEvaluate(inputs);
        } else {
            static bool logged = false;
            if (!logged) {
                logged = true;
                Log("DLSS evaluate without usable depth/motion/jitter parameters; frame generation gets no inputs");
            }
        }
    }
    return g_runtimes[R].evaluateFeature(cmd, handle, parameters, callback);
}

template <int R>
NVSDK_NGX_Result NVSDK_CONV Hook_GetCapabilityParameters(NVSDK_NGX_Parameter** parameters) {
    DepthGuard guard;
    void* const caller = _ReturnAddress();
    const NVSDK_NGX_Result result = g_runtimes[R].getCapabilityParameters(parameters);
    if (guard.outermost() && !IsStreamlineAddress(caller)) {
        static int logged = 0;
        if (logged < 4) {
            ++logged;
            int available = -1, initResult = -1, needsDriver = -1;
            if (NVSDK_NGX_SUCCEED(result) && parameters && *parameters) {
                (*parameters)->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
                (*parameters)->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
                (*parameters)->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
            }
            Log("Game NGX GetCapabilityParameters via %ls -> 0x%X: DLSS available=%d initResult=0x%X needsUpdatedDriver=%d",
                kRuntimeNames[R], static_cast<unsigned>(result), available, static_cast<unsigned>(initResult), needsDriver);
        }
    }
    return result;
}

template <int R, int V>
NVSDK_NGX_Result NVSDK_CONV Hook_Init(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j) {
    DepthGuard guard;
    void* const caller = _ReturnAddress();
    const NVSDK_NGX_Result result = g_runtimes[R].init[V](a, b, c, d, e, f, g, h, i, j);
    if (guard.outermost()) {
        Log("NGX %s via %ls called by %s (first argument 0x%llX) -> 0x%X", kInitNames[V], kRuntimeNames[R], CallerName(caller),
            static_cast<unsigned long long>(a), static_cast<unsigned>(result));
    }
    return result;
}

template <int R>
NVSDK_NGX_Result NVSDK_CONV Hook_Shutdown(uint64_t device) {
    DepthGuard guard;
    void* const caller = _ReturnAddress();
    if (guard.outermost()) Log("NGX Shutdown via %ls called by %s", kRuntimeNames[R], CallerName(caller));
    return g_runtimes[R].shutdown(device);
}

template <int R>
NVSDK_NGX_Result NVSDK_CONV Hook_Shutdown1(uint64_t device) {
    DepthGuard guard;
    void* const caller = _ReturnAddress();
    if (guard.outermost()) Log("NGX Shutdown1 via %ls called by %s", kRuntimeNames[R], CallerName(caller));
    return g_runtimes[R].shutdown1(device);
}

bool HookExport(HMODULE module, const char* name, void* hook, void** original, bool required = true) {
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (!target) {
        if (required) Log("NGX runtime has no export %s", name);
        return !required;
    }
    MH_STATUS status = MH_CreateHook(target, hook, original);
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK) {
        Log("Hooking %s failed: %s", name, MH_StatusToString(status));
        return false;
    }
    return true;
}

template <int R, int V>
void HookInit(HMODULE module) {
    HookExport(module, kInitNames[V], reinterpret_cast<void*>(&Hook_Init<R, V>), reinterpret_cast<void**>(&g_runtimes[R].init[V]), false);
}

template <int R>
void InstallRuntime(HMODULE module) {
    Runtime& rt = g_runtimes[R];
    rt.installed = true;  // one attempt: a half-hooked runtime is not retried
    bool ok = HookExport(module, "NVSDK_NGX_VULKAN_CreateFeature", reinterpret_cast<void*>(&Hook_CreateFeature<R>), reinterpret_cast<void**>(&rt.createFeature));
    ok = HookExport(module, "NVSDK_NGX_VULKAN_CreateFeature1", reinterpret_cast<void*>(&Hook_CreateFeature1<R>), reinterpret_cast<void**>(&rt.createFeature1)) && ok;
    ok = HookExport(module, "NVSDK_NGX_VULKAN_ReleaseFeature", reinterpret_cast<void*>(&Hook_ReleaseFeature<R>), reinterpret_cast<void**>(&rt.releaseFeature)) && ok;
    ok = HookExport(module, "NVSDK_NGX_VULKAN_EvaluateFeature", reinterpret_cast<void*>(&Hook_EvaluateFeature<R>), reinterpret_cast<void**>(&rt.evaluateFeature)) && ok;
    HookExport(module, "NVSDK_NGX_VULKAN_GetCapabilityParameters", reinterpret_cast<void*>(&Hook_GetCapabilityParameters<R>), reinterpret_cast<void**>(&rt.getCapabilityParameters), false);
    HookExport(module, "NVSDK_NGX_VULKAN_Shutdown", reinterpret_cast<void*>(&Hook_Shutdown<R>), reinterpret_cast<void**>(&rt.shutdown), false);
    HookExport(module, "NVSDK_NGX_VULKAN_Shutdown1", reinterpret_cast<void*>(&Hook_Shutdown1<R>), reinterpret_cast<void**>(&rt.shutdown1), false);
    HookInit<R, 0>(module);
    HookInit<R, 1>(module);
    HookInit<R, 2>(module);
    HookInit<R, 3>(module);
    HookInit<R, 4>(module);
    Log("NGX runtime %ls %s", kRuntimeNames[R], ok ? "hooked" : "could not be fully hooked");
}

}  // namespace

bool InstallNgxTap() {
    if (g_runtimes[0].installed && g_runtimes[1].installed) return true;

    const HMODULE modern = GetModuleHandleW(kRuntimeNames[0]);
    if (modern && !g_runtimes[0].installed) InstallRuntime<0>(modern);

    if (!g_runtimes[1].installed) {
        HMODULE legacy = GetModuleHandleW(kRuntimeNames[1]);
        if (!legacy && modern) {
            // The game loads nvngx.dll only when it initialises NGX. Load it first from the same
            // driver folder so its entry points are hooked before the game's first call.
            wchar_t path[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(modern, path, MAX_PATH);
            std::wstring folder(path, length);
            const size_t slash = folder.find_last_of(L'\\');
            if (slash != std::wstring::npos) {
                folder.resize(slash + 1);
                legacy = LoadLibraryExW((folder + kRuntimeNames[1]).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (!legacy) Log("Could not preload %ls%ls (Win32 error %lu)", folder.c_str(), kRuntimeNames[1], GetLastError());
            }
        }
        if (legacy) InstallRuntime<1>(legacy);
    }
    return g_runtimes[0].installed && g_runtimes[1].installed;
}

}  // namespace fg
