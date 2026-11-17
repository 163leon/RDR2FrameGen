#include "config.h"

#include <windows.h>

#include <cwchar>

namespace fg {
namespace {

Config g_config;
constexpr wchar_t kSection[] = L"FrameGen";

int ReadInt(const wchar_t* path, const wchar_t* key, int fallback) {
    // Base 0 so the key code can be written as 0x6A or 106.
    wchar_t text[32]{};
    if (!GetPrivateProfileStringW(kSection, key, L"", text, 32, path)) return fallback;
    wchar_t* end = nullptr;
    const long value = wcstol(text, &end, 0);
    return end == text ? fallback : static_cast<int>(value);
}

float ReadFloat(const wchar_t* path, const wchar_t* key, float fallback) {
    wchar_t text[32]{};
    if (!GetPrivateProfileStringW(kSection, key, L"", text, 32, path)) return fallback;
    wchar_t* end = nullptr;
    const double value = wcstod(text, &end);
    return end == text || value <= 0.0 ? fallback : static_cast<float>(value);
}

}  // namespace

void LoadConfig(const wchar_t* iniPath) {
    g_config.enabled = ReadInt(iniPath, L"Enabled", g_config.enabled) != 0;
    g_config.debugLogging = ReadInt(iniPath, L"DebugLogging", g_config.debugLogging) != 0;
    g_config.toggleKey = ReadInt(iniPath, L"ToggleKey", g_config.toggleKey);
    g_config.cameraNear = ReadFloat(iniPath, L"CameraNear", g_config.cameraNear);
    g_config.cameraFar = ReadFloat(iniPath, L"CameraFar", g_config.cameraFar);
    g_config.cameraDelay = ReadInt(iniPath, L"CameraDelay", g_config.cameraDelay);
    g_config.hudLess = ReadInt(iniPath, L"HudLess", g_config.hudLess) != 0;
}

const Config& Cfg() { return g_config; }

}  // namespace fg
