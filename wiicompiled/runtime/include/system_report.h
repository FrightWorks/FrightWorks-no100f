#pragma once

// What a player's log needs for a bug report beyond the game itself: the operating system, how the game was started
// (Steam, gamescope, an AppImage, RecompLauncher, Wine/Proton) and which SDL video driver it got. One "[system]" block
// near the top of console.log. Nothing personal: no user, host or device names, and no paths.
namespace SystemReport {

// The operating system, memory and the launch environment. Call once, right after the process transcript opens.
void LogHostSystem();

// SDL's video driver (x11, wayland, windows, ...). Call once, after the window exists.
void LogVideoDriver();

}  // namespace SystemReport
