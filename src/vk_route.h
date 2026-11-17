#pragma once

#include <string>

namespace fg {

// Hooks the Vulkan loader exports so that the game's instance, device, surface and swapchain calls go
// through Streamline, as its manual-hooking integration requires. Everything else stays untouched.
// Called from DllMain; Streamline itself is started later, on the game's first vkCreateInstance.
bool InstallVulkanRoute(const std::wstring& gameDirectory);

}  // namespace fg
