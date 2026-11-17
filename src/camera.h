#pragma once

#include <windows.h>

#include <sl.h>

namespace fg {

// Registers the ScriptHook script that reads the game's camera every tick. Call on DLL attach.
void CameraRegisterScript(HMODULE module);
void CameraUnregisterScript(HMODULE module);

// Render thread, once per rendered frame: fills the camera part of the Streamline constants
// (projection, clip-to-previous-clip, position, axes, near/far, field of view) from the game's
// camera. Returns false when no fresh reading exists; the caller then uses a neutral camera.
// `cut` is set when the camera jumped within one tick (cutscene edit, teleport): history must be reset.
bool CameraFillConstants(sl::Constants& constants, float aspect, bool depthInverted, bool reset, bool* cut);

}  // namespace fg
