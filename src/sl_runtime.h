#pragma once

#include <windows.h>

#include <string>

namespace fg {

// Loads sl.interposer.dll from <game>\RDR2FrameGen\ and calls slInit for Vulkan with manual hooking.
// Must run before the game creates its Vulkan device, and not under the Vulkan loader lock.
// Returns false when Streamline cannot be used; the game then runs untouched.
bool SlStart(const std::wstring& gameDirectory);

// Module handle of the loaded sl.interposer.dll, or null.
HMODULE SlInterposer();

// True when the address belongs to a module loaded from the Streamline runtime folder.
bool IsStreamlineAddress(const void* address);

// Called once the game's Vulkan device exists: logs whether DLSS-G and Reflex can run on it and
// returns true when both can.
bool SlReportDevice(void* vkPhysicalDevice);

}  // namespace fg
