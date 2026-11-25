// Services that differ between platforms, behind a small interface (#19). App code calls these functions and doesn't
// know which system it runs on. Each backend implements all of them:
//   win/platform_win.cpp      Windows (what the shell has always used)
//   linux/platform_linux.cpp  Linux (built by CMakeLists.txt)
// The only #ifdef is the choice of backend (in the build), never in app code.
#pragma once
#include <string>

namespace platform {

// Monotonic clock in milliseconds, with fractions (Windows: QueryPerformanceCounter; Linux: steady_clock).
// For animations: step by real time, never per frame.
double NowMs();

// The settings folder (ajustes.txt and the other .txt files), without a trailing separator; empty if unknown.
// Windows: %APPDATA%\Shell. Linux: $XDG_CONFIG_HOME/liquinix (or ~/.config/liquinix). Doesn't create the folder.
std::wstring SettingsDir();

}  // namespace platform
