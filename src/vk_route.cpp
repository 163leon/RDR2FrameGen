// Routes the game's Vulkan entry points through sl.interposer.dll.
//
// How calls are told apart: every hook looks at its return address.
//   - from RDR2.exe      -> the call goes to Streamline's proxy (which then calls the real loader);
//   - from Streamline    -> the real loader, so the proxy's own calls never loop back into itself;
//   - from anything else -> the real loader (ScriptHook, NGX, overlays are left alone).
//
// The game may reach Vulkan through the loader's exports or through pointers it got from
// vkGetInstanceProcAddr / vkGetDeviceProcAddr. Both paths end in the same hook functions below.
//
// Queue-family workaround: sl.interposer 2.14.16 picks the last compute-only queue family for its own
// compute queues. RDR2 already asks for every queue of that family, and the proxy then adds a second
// VkDeviceQueueCreateInfo for the same family, which Vulkan forbids. We hand Streamline a view of the
// families in which compute-only families do not advertise compute; it then puts its queues into the
// graphics family (which also does compute and has spare queues), after the game's own.

#include "vk_route.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <windows.h>

#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <vector>

#include "config.h"
#include "crash_log.h"
#include "frames.h"
#include "hud.h"
#include "log.h"
#include "ngx_tap.h"
#include "sl_runtime.h"

namespace fg {
namespace {

struct VulkanFunctions {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkAcquireNextImage2KHR AcquireNextImage2KHR;  // real only: Streamline has no proxy for it
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
};

VulkanFunctions g_real{};   // the loader's functions (MinHook trampolines)
VulkanFunctions g_proxy{};  // sl.interposer.dll's functions

std::wstring g_gameDirectory;
uintptr_t g_gameBegin = 0, g_gameEnd = 0;
bool g_startAttempted = false;
std::atomic<bool> g_routing{false};  // Streamline is up and the game's calls are being routed
VkInstance g_instance = VK_NULL_HANDLE;  // the game's instance, created through Streamline
VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;  // the adapter the game's device was created on

// Real queue-family queries, fetched from the loader for the instance Streamline asked about.
PFN_vkGetPhysicalDeviceQueueFamilyProperties g_realQueueFamilies = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties2 g_realQueueFamilies2 = nullptr;

bool FromGame(const void* returnAddress) {
    const auto address = reinterpret_cast<uintptr_t>(returnAddress);
    return address >= g_gameBegin && address < g_gameEnd;
}

// The proxy when the game is calling and routing is on, otherwise the real function.
#define FG_TARGET(name) ((g_routing.load(std::memory_order_acquire) && g_proxy.name && FromGame(_ReturnAddress())) ? g_proxy.name : g_real.name)

// ---- queue-family view for Streamline -------------------------------------------------------

VkQueueFlags HideComputeOnlyFamily(VkQueueFlags flags) {
    const bool computeOnly = (flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT);
    return computeOnly ? (flags & ~static_cast<VkQueueFlags>(VK_QUEUE_COMPUTE_BIT)) : flags;
}

void VKAPI_CALL QueueFamiliesForStreamline(VkPhysicalDevice device, uint32_t* count, VkQueueFamilyProperties* families) {
    g_realQueueFamilies(device, count, families);
    if (!families || !count) return;
    for (uint32_t i = 0; i < *count; ++i) families[i].queueFlags = HideComputeOnlyFamily(families[i].queueFlags);
}

void VKAPI_CALL QueueFamilies2ForStreamline(VkPhysicalDevice device, uint32_t* count, VkQueueFamilyProperties2* families) {
    g_realQueueFamilies2(device, count, families);
    if (!families || !count) return;
    for (uint32_t i = 0; i < *count; ++i) {
        auto& flags = families[i].queueFamilyProperties.queueFlags;
        flags = HideComputeOnlyFamily(flags);
    }
}

// ---- hook functions ---------------------------------------------------------------------------

PFN_vkVoidFunction VKAPI_CALL Hook_GetInstanceProcAddr(VkInstance instance, const char* name);
PFN_vkVoidFunction VKAPI_CALL Hook_GetDeviceProcAddr(VkDevice device, const char* name);

void StartStreamlineOnce() {
    if (g_startAttempted) return;
    g_startAttempted = true;
    if (!SlStart(g_gameDirectory)) return;

    const HMODULE interposer = SlInterposer();
    auto resolve = [interposer](const char* name) { return reinterpret_cast<void*>(GetProcAddress(interposer, name)); };
#define FG_PROXY(name) g_proxy.name = static_cast<PFN_vk##name>(resolve("vk" #name))
    FG_PROXY(CreateInstance);
    FG_PROXY(DestroyInstance);
    FG_PROXY(EnumeratePhysicalDevices);
    FG_PROXY(CreateDevice);
    FG_PROXY(DestroyDevice);
    FG_PROXY(CreateWin32SurfaceKHR);
    FG_PROXY(DestroySurfaceKHR);
    FG_PROXY(CreateSwapchainKHR);
    FG_PROXY(DestroySwapchainKHR);
    FG_PROXY(GetSwapchainImagesKHR);
    FG_PROXY(AcquireNextImageKHR);
    FG_PROXY(QueuePresentKHR);
    FG_PROXY(DeviceWaitIdle);
#undef FG_PROXY

    const void* const required[] = {g_proxy.CreateInstance, g_proxy.DestroyInstance, g_proxy.EnumeratePhysicalDevices,
                                    g_proxy.CreateDevice, g_proxy.DestroyDevice, g_proxy.CreateWin32SurfaceKHR,
                                    g_proxy.DestroySurfaceKHR, g_proxy.CreateSwapchainKHR, g_proxy.DestroySwapchainKHR,
                                    g_proxy.GetSwapchainImagesKHR, g_proxy.AcquireNextImageKHR, g_proxy.QueuePresentKHR,
                                    g_proxy.DeviceWaitIdle};
    for (const void* function : required) {
        if (!function) {
            Log("sl.interposer.dll lacks an expected Vulkan export; routing stays off");
            return;
        }
    }
    g_routing.store(true, std::memory_order_release);
    Log("Vulkan routing through Streamline is active");
}

VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance) {
    if (!FromGame(_ReturnAddress())) return g_real.CreateInstance(info, allocator, instance);

    // Streamline must be initialised before the device exists, and outside the loader lock:
    // here we are in the game's own call, not inside a vkGet*ProcAddr lookup.
    StartStreamlineOnce();
    if (!g_routing.load(std::memory_order_acquire)) return g_real.CreateInstance(info, allocator, instance);

    const uint32_t api = info->pApplicationInfo ? info->pApplicationInfo->apiVersion : 0;
    Log("Game vkCreateInstance: api=%u.%u.%u extensions=%u layers=%u", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
        VK_API_VERSION_PATCH(api), info->enabledExtensionCount, info->enabledLayerCount);
    const VkResult result = g_proxy.CreateInstance(info, allocator, instance);
    Log("Game vkCreateInstance -> %d", result);
    if (result == VK_SUCCESS) g_instance = *instance;
    return result;
}

void VKAPI_CALL Hook_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    FG_TARGET(DestroyInstance)(instance, allocator);
}

VkResult VKAPI_CALL Hook_EnumeratePhysicalDevices(VkInstance instance, uint32_t* count, VkPhysicalDevice* devices) {
    return FG_TARGET(EnumeratePhysicalDevices)(instance, count, devices);
}

void LogQueueRequest(const char* who, const VkDeviceCreateInfo* info) {
    char text[256];
    int used = 0;
    for (uint32_t i = 0; i < info->queueCreateInfoCount && used < static_cast<int>(sizeof(text)) - 24; ++i) {
        used += snprintf(text + used, sizeof(text) - used, " family%u x%u", info->pQueueCreateInfos[i].queueFamilyIndex,
                         info->pQueueCreateInfos[i].queueCount);
    }
    Log("%s vkCreateDevice: queues:%s; extensions=%u", who, used ? text : " none", info->enabledExtensionCount);
}

VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* device) {
    const void* caller = _ReturnAddress();
    if (!g_routing.load(std::memory_order_acquire) || !FromGame(caller)) {
        // Streamline's proxy lands here with the final create info; log it to verify the queue layout.
        if (IsStreamlineAddress(caller)) LogQueueRequest("Streamline", info);
        return g_real.CreateDevice(physicalDevice, info, allocator, device);
    }

    LogQueueRequest("Game", info);

    // Streamline learns which instance a physical device belongs to from its vkEnumeratePhysicalDevices
    // proxy. RDR2 finds its adapter some other way (the proxy never sees the call), which left
    // Streamline with a null instance and a crash in plugin start-up. Run the enumeration for it.
    uint32_t count = 0;
    bool known = false;
    if (g_instance && g_proxy.EnumeratePhysicalDevices(g_instance, &count, nullptr) == VK_SUCCESS && count) {
        std::vector<VkPhysicalDevice> devices(count);
        g_proxy.EnumeratePhysicalDevices(g_instance, &count, devices.data());
        for (uint32_t i = 0; i < count; ++i) known = known || devices[i] == physicalDevice;
    }
    Log("Physical devices enumerated for Streamline: %u, the game's device is %s", count, known ? "among them" : "NOT among them");
    if (!known) return g_real.CreateDevice(physicalDevice, info, allocator, device);

    const VkResult result = g_proxy.CreateDevice(physicalDevice, info, allocator, device);
    Log("Game vkCreateDevice -> %d", result);
    if (result == VK_SUCCESS) g_physicalDevice = physicalDevice;
    if (result == VK_SUCCESS && SlReportDevice(physicalDevice)) {
        // Streamline has loaded the NGX runtime by now; hook it before the game creates its upscaler.
        InstallNgxTap();
        FramesEnable();
    }
    return result;
}

void VKAPI_CALL Hook_DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    FG_TARGET(DestroyDevice)(device, allocator);
}

// Alt+Enter reaches the game as a posted key message; this runs before the game (or an overlay
// that subclassed its window) sees it, so generation is already off when the window changes.
HHOOK g_messageHook = nullptr;
LRESULT CALLBACK MessageHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == PM_REMOVE) {
        const MSG* message = reinterpret_cast<const MSG*>(lParam);
        if (message->message == WM_SYSKEYDOWN && message->wParam == VK_RETURN && !(message->lParam & (1 << 30))) {
            FramesQuiesce("Alt+Enter");
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

VkResult VKAPI_CALL Hook_CreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    const auto target = FG_TARGET(CreateWin32SurfaceKHR);
    if (target == g_proxy.CreateWin32SurfaceKHR && info && info->hwnd && !g_messageHook) {
        g_messageHook = SetWindowsHookExW(WH_GETMESSAGE, &MessageHook, nullptr, GetWindowThreadProcessId(info->hwnd, nullptr));
        if (!g_messageHook) Log("Message hook on the game window failed (Win32 error %lu)", GetLastError());
    }
    const VkResult result = target(instance, info, allocator, surface);
    if (target == g_proxy.CreateWin32SurfaceKHR) {
        RECT client{};
        if (info && info->hwnd) GetClientRect(info->hwnd, &client);
        LogDebug("Game vkCreateWin32SurfaceKHR: window %p client %ldx%ld -> %d, surface %p", info ? static_cast<void*>(info->hwnd) : nullptr,
                 client.right, client.bottom, result, surface ? reinterpret_cast<void*>(*surface) : nullptr);
    }
    return result;
}

void VKAPI_CALL Hook_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator) {
    const auto target = FG_TARGET(DestroySurfaceKHR);
    if (target == g_proxy.DestroySurfaceKHR) {
        // The game has been seen destroying the same surface in an endless loop after Alt+Enter.
        static VkSurfaceKHR last = VK_NULL_HANDLE;
        static unsigned repeats = 0;
        repeats = surface == last ? repeats + 1 : 0;
        last = surface;
        if (repeats < 3) Log("Game vkDestroySurfaceKHR: surface %p", reinterpret_cast<void*>(surface));
        if (repeats == 3) LogAllThreadStacks("the game destroys the same surface again and again");
        if (repeats == 2000) LogAllThreadStacks("still destroying the same surface");
        if (repeats >= 3) return;  // destroying a destroyed surface again is invalid; skip it
    }
    target(instance, surface, allocator);
}

// Diagnostics for the game's surface queries (it re-creates the surface when the window mode changes).
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_realSurfaceCapabilities = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR g_realSurfaceCapabilities2 = nullptr;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR g_realSurfaceSupport = nullptr;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR g_realSurfacePresentModes = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR g_realSurfaceFormats = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormats2KHR g_realSurfaceFormats2 = nullptr;

VkResult VKAPI_CALL Wrap_SurfaceCapabilities(VkPhysicalDevice device, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* capabilities) {
    const VkResult result = g_realSurfaceCapabilities(device, surface, capabilities);
    LogDebug("Game vkGetPhysicalDeviceSurfaceCapabilitiesKHR -> %d, extent %ux%u, images %u-%u", result, capabilities->currentExtent.width,
             capabilities->currentExtent.height, capabilities->minImageCount, capabilities->maxImageCount);
    return result;
}

VkResult VKAPI_CALL Wrap_SurfaceCapabilities2(VkPhysicalDevice device, const VkPhysicalDeviceSurfaceInfo2KHR* info, VkSurfaceCapabilities2KHR* capabilities) {
    const VkResult result = g_realSurfaceCapabilities2(device, info, capabilities);
    const auto* chained = static_cast<const VkBaseInStructure*>(info->pNext);
    LogDebug("Game vkGetPhysicalDeviceSurfaceCapabilities2KHR -> %d, extent %ux%u, first chained struct %d", result,
             capabilities->surfaceCapabilities.currentExtent.width, capabilities->surfaceCapabilities.currentExtent.height,
             chained ? static_cast<int>(chained->sType) : 0);
    return result;
}

VkResult VKAPI_CALL Wrap_SurfaceSupport(VkPhysicalDevice device, uint32_t family, VkSurfaceKHR surface, VkBool32* supported) {
    const VkResult result = g_realSurfaceSupport(device, family, surface, supported);
    LogDebug("Game vkGetPhysicalDeviceSurfaceSupportKHR(family %u) -> %d, supported=%u", family, result, *supported);
    return result;
}

VkResult VKAPI_CALL Wrap_SurfacePresentModes(VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t* count, VkPresentModeKHR* modes) {
    const VkResult result = g_realSurfacePresentModes(device, surface, count, modes);
    LogDebug("Game vkGetPhysicalDeviceSurfacePresentModesKHR -> %d, count %u", result, *count);
    return result;
}

VkResult VKAPI_CALL Wrap_SurfaceFormats(VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t* count, VkSurfaceFormatKHR* formats) {
    const VkResult result = g_realSurfaceFormats(device, surface, count, formats);
    LogDebug("Game vkGetPhysicalDeviceSurfaceFormatsKHR -> %d, count %u", result, *count);
    return result;
}

VkResult VKAPI_CALL Wrap_SurfaceFormats2(VkPhysicalDevice device, const VkPhysicalDeviceSurfaceInfo2KHR* info, uint32_t* count, VkSurfaceFormat2KHR* formats) {
    const VkResult result = g_realSurfaceFormats2(device, info, count, formats);
    LogDebug("Game vkGetPhysicalDeviceSurfaceFormats2KHR -> %d, count %u", result, *count);
    return result;
}

PFN_vkVoidFunction WrapSurfaceQuery(const char* name, PFN_vkVoidFunction real) {
    if (!real || !Cfg().debugLogging) return real;
#define FG_QUERY(vkName, slot, wrapper)                                 \
    if (strcmp(name, vkName) == 0) {                                    \
        slot = reinterpret_cast<decltype(slot)>(real);                  \
        return reinterpret_cast<PFN_vkVoidFunction>(&wrapper);          \
    }
    FG_QUERY("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", g_realSurfaceCapabilities, Wrap_SurfaceCapabilities)
    FG_QUERY("vkGetPhysicalDeviceSurfaceCapabilities2KHR", g_realSurfaceCapabilities2, Wrap_SurfaceCapabilities2)
    FG_QUERY("vkGetPhysicalDeviceSurfaceSupportKHR", g_realSurfaceSupport, Wrap_SurfaceSupport)
    FG_QUERY("vkGetPhysicalDeviceSurfacePresentModesKHR", g_realSurfacePresentModes, Wrap_SurfacePresentModes)
    FG_QUERY("vkGetPhysicalDeviceSurfaceFormatsKHR", g_realSurfaceFormats, Wrap_SurfaceFormats)
    FG_QUERY("vkGetPhysicalDeviceSurfaceFormats2KHR", g_realSurfaceFormats2, Wrap_SurfaceFormats2)
#undef FG_QUERY
    return real;
}

// Full-screen exclusive mode is taken and released on the real swapchain, behind Streamline's back.
PFN_vkAcquireFullScreenExclusiveModeEXT g_realAcquireFullScreen = nullptr;
PFN_vkReleaseFullScreenExclusiveModeEXT g_realReleaseFullScreen = nullptr;

VkResult VKAPI_CALL Wrap_AcquireFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain) {
    FramesQuiesce("vkAcquireFullScreenExclusiveModeEXT");
    const VkResult result = g_realAcquireFullScreen(device, swapchain);
    Log("Game vkAcquireFullScreenExclusiveModeEXT -> %d", result);
    return result;
}

VkResult VKAPI_CALL Wrap_ReleaseFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain) {
    FramesQuiesce("vkReleaseFullScreenExclusiveModeEXT");
    const VkResult result = g_realReleaseFullScreen(device, swapchain);
    Log("Game vkReleaseFullScreenExclusiveModeEXT -> %d", result);
    return result;
}

VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    const auto target = FG_TARGET(CreateSwapchainKHR);
    if (target != g_proxy.CreateSwapchainKHR) return target(device, info, allocator, swapchain);
    FramesQuiesce("vkCreateSwapchainKHR");

    // The HUD-less copy reads the swapchain image, which the game itself never does.
    VkSwapchainCreateInfoKHR patched = *info;
    patched.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult result = target(device, &patched, allocator, swapchain);
    const bool readable = result == VK_SUCCESS;
    if (result != VK_SUCCESS && patched.imageUsage != info->imageUsage) result = target(device, info, allocator, swapchain);
    Log("Game vkCreateSwapchainKHR: %ux%u format=%d colorSpace=%d minImages=%u presentMode=%d usage=0x%X (readable=%d) -> %d",
        info->imageExtent.width, info->imageExtent.height, info->imageFormat, info->imageColorSpace, info->minImageCount,
        info->presentMode, info->imageUsage, readable, result);
    if (result == VK_SUCCESS && readable && g_instance && g_physicalDevice) {
        const auto memoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            g_real.GetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto queueFamilies = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            g_real.GetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        if (memoryProperties && queueFamilies) {
            VkPhysicalDeviceMemoryProperties memory{};
            memoryProperties(g_physicalDevice, &memory);
            uint32_t families = 0;
            queueFamilies(g_physicalDevice, &families, nullptr);
            HudOnSwapchainCreated(device, memory, families, g_real.GetDeviceProcAddr, info->imageFormat, info->imageExtent.width,
                                  info->imageExtent.height);
        }
    }
    return result;
}

void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator) {
    const auto target = FG_TARGET(DestroySwapchainKHR);
    if (target == g_proxy.DestroySwapchainKHR) FramesQuiesce("vkDestroySwapchainKHR");
    target(device, swapchain, allocator);
}

VkResult VKAPI_CALL Hook_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images) {
    const auto target = FG_TARGET(GetSwapchainImagesKHR);
    const VkResult result = target(device, swapchain, count, images);
    if (target == g_proxy.GetSwapchainImagesKHR && images && count && (result == VK_SUCCESS || result == VK_INCOMPLETE)) {
        HudOnSwapchainImages(images, *count);
    }
    return result;
}

VkResult VKAPI_CALL Hook_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* imageIndex) {
    const auto target = FG_TARGET(AcquireNextImageKHR);
    if (target == g_proxy.AcquireNextImageKHR) {
        static bool first = true;
        if (first) {
            first = false;
            Log("First game vkAcquireNextImageKHR through Streamline: semaphore=%d fence=%d", semaphore != VK_NULL_HANDLE, fence != VK_NULL_HANDLE);
        }
    }
    return target(device, swapchain, timeout, semaphore, fence, imageIndex);
}

// Streamline does not intercept the Vulkan 1.1 variant. Left alone, the game would take images
// straight from the real swapchain, behind the proxy swapchain Streamline handed it.
VkResult VKAPI_CALL Hook_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* imageIndex) {
    if (!g_routing.load(std::memory_order_acquire) || !FromGame(_ReturnAddress())) return g_real.AcquireNextImage2KHR(device, info, imageIndex);
    static bool first = true;
    if (first) {
        first = false;
        Log("Game uses vkAcquireNextImage2KHR; forwarding to Streamline's vkAcquireNextImageKHR");
    }
    return g_proxy.AcquireNextImageKHR(device, info->swapchain, info->timeout, info->semaphore, info->fence, imageIndex);
}

VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    const auto target = FG_TARGET(QueuePresentKHR);
    if (target != g_proxy.QueuePresentKHR) return target(queue, info);

    static bool first = true;
    if (first) {
        first = false;
        Log("First game present through Streamline: swapchains=%u waitSemaphores=%u", info->swapchainCount,
            info->waitSemaphoreCount);
    }
    InstallNgxTap();  // no-op once installed; retried in case the NGX runtime loads late
    FramePresentBegin(info->swapchainCount && info->pImageIndices ? info->pImageIndices[0] : UINT32_MAX);
    HudOnPresent();
    const VkResult result = target(queue, info);
    FramePresentEnd();
    return result;
}

VkResult VKAPI_CALL Hook_DeviceWaitIdle(VkDevice device) { return FG_TARGET(DeviceWaitIdle)(device); }

// Names the game must receive our hooks for, whichever way it looks them up.
struct RoutedName {
    const char* name;
    PFN_vkVoidFunction hook;
};
#define FG_ROUTED(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(&Hook_##name)}
const RoutedName kInstanceNames[] = {
    FG_ROUTED(GetInstanceProcAddr), FG_ROUTED(GetDeviceProcAddr), FG_ROUTED(CreateInstance),
    FG_ROUTED(DestroyInstance),     FG_ROUTED(EnumeratePhysicalDevices), FG_ROUTED(CreateDevice),
    FG_ROUTED(DestroyDevice),       FG_ROUTED(CreateWin32SurfaceKHR),    FG_ROUTED(DestroySurfaceKHR),
};
const RoutedName kDeviceNames[] = {
    FG_ROUTED(GetDeviceProcAddr),    FG_ROUTED(CreateSwapchainKHR),  FG_ROUTED(DestroySwapchainKHR),
    FG_ROUTED(GetSwapchainImagesKHR), FG_ROUTED(AcquireNextImageKHR), FG_ROUTED(QueuePresentKHR),
    FG_ROUTED(DeviceWaitIdle),        FG_ROUTED(AcquireNextImage2KHR),
};

// Diagnostics: which presentation entry points the game asks the loader for.
void LogLookup(const char* how, const char* name) {
    if (strstr(name, "Swapchain") || strstr(name, "Acquire") || strstr(name, "Present") || strstr(name, "Surface")) {
        LogDebug("Game looked up %s via %s", name, how);
    }
}
#undef FG_ROUTED

template <size_t N>
PFN_vkVoidFunction FindRouted(const RoutedName (&table)[N], const char* name) {
    for (const RoutedName& entry : table) {
        if (strcmp(entry.name, name) == 0) return entry.hook;
    }
    return nullptr;
}

PFN_vkVoidFunction VKAPI_CALL Hook_GetInstanceProcAddr(VkInstance instance, const char* name) {
    const void* caller = _ReturnAddress();
    if (!name || !g_routing.load(std::memory_order_acquire)) return g_real.GetInstanceProcAddr(instance, name);

    if (FromGame(caller)) {
        LogLookup("vkGetInstanceProcAddr", name);
        if (PFN_vkVoidFunction hook = FindRouted(kInstanceNames, name)) return hook;
        if (PFN_vkVoidFunction hook = FindRouted(kDeviceNames, name)) return hook;
        return HudWrap(name, WrapSurfaceQuery(name, g_real.GetInstanceProcAddr(instance, name)));
    }

    if (instance && IsStreamlineAddress(caller)) {
        if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0) {
            g_realQueueFamilies = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(g_real.GetInstanceProcAddr(instance, name));
            return g_realQueueFamilies ? reinterpret_cast<PFN_vkVoidFunction>(&QueueFamiliesForStreamline) : nullptr;
        }
        if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2") == 0) {
            g_realQueueFamilies2 = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(g_real.GetInstanceProcAddr(instance, name));
            return g_realQueueFamilies2 ? reinterpret_cast<PFN_vkVoidFunction>(&QueueFamilies2ForStreamline) : nullptr;
        }
    }
    return g_real.GetInstanceProcAddr(instance, name);
}

PFN_vkVoidFunction VKAPI_CALL Hook_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (name && g_routing.load(std::memory_order_acquire) && FromGame(_ReturnAddress())) {
        LogLookup("vkGetDeviceProcAddr", name);
        if (PFN_vkVoidFunction hook = FindRouted(kDeviceNames, name)) return hook;
        const PFN_vkVoidFunction real = g_real.GetDeviceProcAddr(device, name);
        if (real && strcmp(name, "vkAcquireFullScreenExclusiveModeEXT") == 0) {
            g_realAcquireFullScreen = reinterpret_cast<PFN_vkAcquireFullScreenExclusiveModeEXT>(real);
            return reinterpret_cast<PFN_vkVoidFunction>(&Wrap_AcquireFullScreenExclusiveModeEXT);
        }
        if (real && strcmp(name, "vkReleaseFullScreenExclusiveModeEXT") == 0) {
            g_realReleaseFullScreen = reinterpret_cast<PFN_vkReleaseFullScreenExclusiveModeEXT>(real);
            return reinterpret_cast<PFN_vkVoidFunction>(&Wrap_ReleaseFullScreenExclusiveModeEXT);
        }
        return HudWrap(name, real);
    }
    return g_real.GetDeviceProcAddr(device, name);
}

bool HookExport(HMODULE vulkan, const char* name, void* hook, void** original) {
    void* target = reinterpret_cast<void*>(GetProcAddress(vulkan, name));
    if (!target) {
        Log("vulkan-1.dll has no export %s", name);
        return false;
    }
    const MH_STATUS status = MH_CreateHook(target, hook, original);
    if (status != MH_OK) {
        Log("MH_CreateHook(%s) failed: %s", name, MH_StatusToString(status));
        return false;
    }
    return true;
}

}  // namespace

bool InstallVulkanRoute(const std::wstring& gameDirectory) {
    g_gameDirectory = gameDirectory;

    const HMODULE game = GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(game) + dos->e_lfanew);
    g_gameBegin = reinterpret_cast<uintptr_t>(game);
    g_gameEnd = g_gameBegin + nt->OptionalHeader.SizeOfImage;

    const HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!vulkan) {
        Log("vulkan-1.dll could not be loaded (Win32 error %lu); nothing hooked", GetLastError());
        return false;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize failed: %s", MH_StatusToString(status));
        return false;
    }

    bool ok = true;
#define FG_HOOK(name) ok = HookExport(vulkan, "vk" #name, reinterpret_cast<void*>(&Hook_##name), reinterpret_cast<void**>(&g_real.name)) && ok
    FG_HOOK(GetInstanceProcAddr);
    FG_HOOK(GetDeviceProcAddr);
    FG_HOOK(CreateInstance);
    FG_HOOK(DestroyInstance);
    FG_HOOK(EnumeratePhysicalDevices);
    FG_HOOK(CreateDevice);
    FG_HOOK(DestroyDevice);
    FG_HOOK(CreateWin32SurfaceKHR);
    FG_HOOK(DestroySurfaceKHR);
    FG_HOOK(CreateSwapchainKHR);
    FG_HOOK(DestroySwapchainKHR);
    FG_HOOK(GetSwapchainImagesKHR);
    FG_HOOK(AcquireNextImageKHR);
    FG_HOOK(AcquireNextImage2KHR);
    FG_HOOK(QueuePresentKHR);
    FG_HOOK(DeviceWaitIdle);
#undef FG_HOOK
    if (!ok) {
        Log("Not every Vulkan export could be hooked; leaving the game untouched");
        MH_Uninitialize();
        return false;
    }

    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        Log("MH_EnableHook failed: %s", MH_StatusToString(status));
        return false;
    }
    Log("Vulkan loader hooked (%p); waiting for the game's vkCreateInstance", static_cast<void*>(vulkan));
    return true;
}

}  // namespace fg
