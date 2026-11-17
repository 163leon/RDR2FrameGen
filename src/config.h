#pragma once

namespace fg {

struct Config {
    bool enabled = true;        // master switch: false leaves the game untouched
    bool debugLogging = false;  // detailed per-frame diagnostics in the log
    int toggleKey = 0x24;       // virtual key that turns generation on/off in game (Home)
    // The game exposes no near/far clip distances; these are the engine's usual values. With the
    // game's reversed depth only the near plane matters in practice.
    float cameraNear = 0.15f;
    float cameraFar = 10000.0f;
    int cameraDelay = 1;        // how many script ticks behind the newest camera reading frames stay
    bool hudLess = true;        // give DLSS-G the frame without interface (0 only for comparison)
};

// Reads RDR2FrameGen.ini; a missing file or key keeps the default.
void LoadConfig(const wchar_t* iniPath);

const Config& Cfg();

}  // namespace fg
