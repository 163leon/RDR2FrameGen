# RDR2FrameGen

DLSS Frame Generation for Red Dead Redemption 2 (story mode, Vulkan, RTX 40/50).

The game has DLSS upscaling but no frame generation. This mod plugs NVIDIA's real DLSS-G into the
game through NVIDIA Streamline and feeds it the game's real data:

- **depth and motion vectors** taken from the game's own DLSS call;
- **the game's camera** (position, rotation, field of view, read every tick through ScriptHookRDR2),
  so DLSS-G knows how the view moved instead of guessing from a placeholder camera;
- **the frame without the interface**, copied right after the scene pass and before the HUD is drawn,
  so the minimap, prompts and subtitles do not smear;
- **history resets** on camera cuts (cutscene edits, fast travel, teleports).

The result is 2x frame generation: one generated frame per rendered frame.

Story mode only. **Do not go into Red Dead Online with this mod installed.**

Русская версия: [README.ru.md](README.ru.md).

## Requirements

- NVIDIA RTX 40 or 50 series, a recent driver.
- In the game: API **Vulkan**, **DLSS** on (any quality mode), V-Sync off.
- Windows: "Hardware-accelerated GPU scheduling" on.
- [ScriptHookRDR2](http://dev-c.com/rdr2/scripthookrdr2/) with its ASI loader (`dinput8.dll`), or another ASI loader plus ScriptHookRDR2.

## Installing

Close the game first.

1. Download `RDR2FrameGen-<version>.zip` from [Releases](../../releases) and unpack it anywhere.
2. Run `install.bat`.

The installer finds the game through Steam or Rockstar registry entries (pass
`-Game "<folder with RDR2.exe>"` otherwise) and puts into the game folder:

- `RDR2FrameGen.asi` and `RDR2FrameGen.ini` (an existing ini is kept);
- a folder `RDR2FrameGen\` with NVIDIA's Streamline libraries. They are not bundled with the mod:
  the installer downloads NVIDIA's official Streamline 2.14.16 release from GitHub (about 280 MB,
  once; cached in `%LOCALAPPDATA%\RDR2FrameGen`) and extracts only the seven files it needs. It also
  copies the game's own `nvngx_dlss.dll` there, because Streamline makes DLSS look for its libraries
  in that folder. **Run the installer again after a game update or after swapping the DLSS version.**

To uninstall, delete `RDR2FrameGen.asi`, `RDR2FrameGen.ini`, `RDR2FrameGen.log` and the
`RDR2FrameGen\` folder from the game folder.

## Using it

- Frame generation turns on by itself half a second after you are in the world, and off on screens
  without a 3D scene (loading screens, videos), while the game window is not focused, and around
  window mode changes.
- **Home** turns it off and on (the key is set in the ini).
- There is no menu or overlay. `RDR2FrameGen.log` next to the game says what the mod is doing. Check
  that it is working with an FPS counter that counts presented frames (NVIDIA overlay, RTSS).

## Settings (`RDR2FrameGen.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `1` | `0` leaves the game untouched (only crash reports stay on) |
| `ToggleKey` | `0x24` (Home) | key that turns frame generation off and on |
| `HudLess` | `1` | give DLSS-G the frame without the interface; `0` only to compare |
| `CameraNear`, `CameraFar` | `0.15`, `10000` | camera clip planes in metres; the game does not expose them |
| `CameraDelay` | `1` | how many game ticks the camera reading lags the newest one (keeps readings in step with frames) |
| `DebugLogging` | `0` | detailed diagnostics in the log; the End key then logs the render passes of two frames |

## Limitations

- Grass, particles and water can still ghost a little: the game has no motion vectors for them.
- The map and the pause menu count as interface and run at the rendered frame rate.
- Below about 45 rendered FPS, artifacts are more visible. That is a property of DLSS-G itself.
- Only 2x. No multi frame generation, no support for other GPUs.
- Tested with RDR2 1.0.1491.50 (Steam), Streamline 2.14.16, RTX 4070, driver 621.24, alongside about
  30 other mods. Other game builds may work but are untested.

## If the game crashes

The mod writes any crash in the game process to `RDR2FrameGen.log` (module and offset, call stack),
also with `Enabled=0`. Please attach that log to an issue. A crash whose stack shows only `RDR2.exe`
is usually the game or another mod; `Enabled=0` tells for sure.

## Building from source

Needs Visual Studio 2022 (C++ desktop workload) and CMake 3.25+.

```bat
powershell -ExecutionPolicy Bypass -File tools\fetch-streamline.ps1
cmake -S . -B build -A x64
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File install.ps1
```

`tools\fetch-streamline.ps1` unpacks the Streamline SDK into `external\streamline` (or point CMake
at your own copy with `-DSTREAMLINE_SDK=<folder>`). Vulkan headers come from `%VULKAN_SDK%` if a
Vulkan SDK is installed, otherwise CMake downloads them. `install.ps1 -Development` installs
Streamline's development libraries, which add NVIDIA's debug overlay for checking the inputs.
`tools\package.ps1` builds the release zip.

## How it works

| File | Job |
|---|---|
| `src/vk_route.cpp` | Hooks `vulkan-1.dll` so that the game's instance, device and swapchain go through Streamline's interposer, while Streamline's own calls go to the real loader. Works around three problems: Streamline's queue allocation for RDR2's queue layout, the game never enumerating physical devices through the proxy, and the game acquiring images with `vkAcquireNextImage2KHR`, which Streamline does not intercept. |
| `src/sl_runtime.cpp` | Loads Streamline, DLSS-G, Reflex and PCL. |
| `src/ngx_tap.cpp` | Intercepts the game's DLSS evaluate in both NGX runtimes to pick up depth, motion vectors, jitter and flags. |
| `src/frames.cpp` | One Streamline frame per game frame: latency markers, resource tags, constants, and when generation may run. |
| `src/camera.cpp` | Reads the final rendered camera every tick and builds the view and projection matrices for Streamline in double precision. |
| `src/hud.cpp` | Finds the scene pass in the back buffer and copies the frame before the HUD is drawn. |
| `src/crash_log.cpp` | Crash and hang reports. |

`NOTES.md` is the development log (in Russian): what was found out about the game, Streamline and
DLSS-G along the way, including the dead ends.

## Credits and licence

MIT licence, see `LICENSE`. Third-party components and their licences: `THIRD_PARTY.md`.

Written with the help of Claude Code (an AI coding assistant) and tested in the game by a human.

Not affiliated with or endorsed by NVIDIA, Rockstar Games or Take-Two Interactive.
