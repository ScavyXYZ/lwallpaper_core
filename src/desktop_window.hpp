#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <utility>

namespace livewallpaper::detail {

struct ScreenSize {
    int width;
    int height;
};

ScreenSize physicalScreenSize();

// Finds the WorkerW window that hosts the desktop background, falling back to
// Progman when no suitably sized WorkerW exists. Returns nullptr when Progman
// itself cannot be found.
HWND findWallpaperHost(int screenWidth, int screenHeight);

// Creates the child window that the wallpaper is drawn into. The window is a
// child of `host` and covers it entirely.
HWND createWallpaperChild(HWND host, int width, int height);

// Drains the calling thread's message queue so the wallpaper window keeps
// responding. Sets `cancelled` and returns false if WM_QUIT is seen.
bool pumpMessages(std::atomic<bool>& cancelled);

} // namespace livewallpaper::detail
