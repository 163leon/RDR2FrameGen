#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

namespace fg {

// Called when the game resolves a device function: returns a tracking wrapper for the functions the
// back-buffer tracking needs (remembering the real one), or `real` unchanged.
PFN_vkVoidFunction HudWrap(const char* name, PFN_vkVoidFunction real);

// The swapchain images the game was given (Streamline's proxies while frame generation is loaded).
void HudOnSwapchainImages(const VkImage* images, uint32_t count);

// The game's swapchain was (re)created: prepares one HUD-less copy target per swapchain image.
// `resolve` is the real vkGetDeviceProcAddr, `queueFamilies` the number of queue families of the device.
void HudOnSwapchainCreated(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, uint32_t queueFamilies,
                           PFN_vkGetDeviceProcAddr resolve, VkFormat format, uint32_t width, uint32_t height);

// Present thread, once per game present.
void HudOnPresent();

// The frame without the game's interface, copied from the swapchain image right after the game wrote
// the finished scene into it and before it drew the HUD on top.
struct HudLessImage {
    VkImage image{};
    VkImageView view{};
    VkFormat format{};
    VkImageLayout layout{};
    VkImageUsageFlags usage{};
    uint32_t width{}, height{};
};

// Present thread: the copy made for the swapchain image about to be presented, if the game drew the
// scene into it this frame. Returns false when there is none (then nothing should be tagged).
bool HudLessForPresent(uint32_t imageIndex, HudLessImage& out);

// Diagnostics: write the back-buffer activity of the next `frames` frames to the log.
void HudStartProbe(int frames);

}  // namespace fg
