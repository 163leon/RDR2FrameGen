# Third-party components

## Included in this repository

- **MinHook** (`third_party/minhook`) by Tsuda Kageyu, BSD 2-Clause license, see
  `third_party/minhook/LICENSE.txt`. Used to hook the exports of `vulkan-1.dll`.

## Used, but not included

- **NVIDIA Streamline SDK 2.14.16** (headers, import library and runtime: `sl.interposer.dll`,
  `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll`,
  `NvLowLatencyVk.dll`). Not redistributed here. `tools/fetch-streamline.ps1` (for building) and
  `install.ps1` (for installing) download NVIDIA's official release from
  <https://github.com/NVIDIA-RTX/Streamline/releases> on the user's machine. The Streamline SDK and
  the DLSS libraries are subject to NVIDIA's license terms, included in that release.
- **The game's own DLSS library** (`nvngx_dlss.dll`). `install.ps1` copies the file that ships with
  the user's copy of the game into the mod's runtime folder; it is not distributed with the mod.
- **ScriptHookRDR2** by Alexander Blade (<http://dev-c.com>). Required at run time and installed
  separately by the user. The mod declares the six exports it calls itself (`src/scripthook.h`,
  `src/scripthook.def`); no part of the ScriptHook SDK is included.
- **Vulkan headers** (Khronos, Apache 2.0). Taken from an installed Vulkan SDK, or fetched by CMake
  from <https://github.com/KhronosGroup/Vulkan-Headers> at build time.

Red Dead Redemption 2 is a trademark of Take-Two Interactive / Rockstar Games. NVIDIA, DLSS and
Streamline are trademarks of NVIDIA Corporation. This project is not affiliated with or endorsed by
either company.
