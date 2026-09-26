#include "desktop_window.hpp"
#include "log.hpp"

#include <cstdlib>

namespace livewallpaper::detail {
namespace {

constexpr wchar_t kWallpaperClass[] = L"LiveWallpaperChild";

LRESULT CALLBACK wallpaperChildProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

ScreenSize physicalScreenSize() {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth > 0) {
        return { static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight) };
    }
    return { GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
}

HWND findWallpaperHost(int screenWidth, int screenHeight) {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) {
        LW_LOG_ERROR("Progman window not found");
        return nullptr;
    }

    // Asks the shell to split Progman, which is what creates the WorkerW that
    // sits behind the desktop icons.
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);

    HWND worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
    while (worker) {
        RECT r{};
        GetWindowRect(worker, &r);
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        if (std::abs(w - screenWidth) <= 10 && std::abs(h - screenHeight) <= 10) {
            LW_LOG_INFO("Using WorkerW as wallpaper host");
            return worker;
        }
        worker = FindWindowExW(progman, worker, L"WorkerW", nullptr);
    }

    LW_LOG_WARN("No full-screen WorkerW found, falling back to Progman");
    return progman;
}

HWND createWallpaperChild(HWND host, int width, int height) {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wallpaperChildProc;
    wc.hInstance = inst;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWallpaperClass;

    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LW_LOG_ERROR("RegisterClassEx failed, error=" << err);
            return nullptr;
        }
    }

    HWND hwnd = CreateWindowExW(0, kWallpaperClass, L"wallpaper",
                                WS_CHILD | WS_VISIBLE,
                                0, 0, width, height,
                                host, nullptr, inst, nullptr);
    if (!hwnd) {
        LW_LOG_ERROR("CreateWindowEx failed, error=" << GetLastError());
        return nullptr;
    }
    return hwnd;
}

bool pumpMessages(std::atomic<bool>& cancelled) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            cancelled = true;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

} // namespace livewallpaper::detail
