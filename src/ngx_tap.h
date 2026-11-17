#pragma once

namespace fg {

// Hooks the NVIDIA NGX runtime (_nvngx.dll) so that every DLSS upscaling pass the game runs also
// reports its depth, motion vectors and jitter to frame generation. The game's own DLSS is unchanged.
// Safe to call repeatedly; returns true once the hooks are in place.
bool InstallNgxTap();

}  // namespace fg
