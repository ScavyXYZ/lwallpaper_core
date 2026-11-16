# livewallpaper core

Windows C++17 static library for looping desktop video wallpapers. **No Qt,
GUI, wallpaper collection, installer, or user settings live in this repository.**
The GUI application is maintained separately at
https://github.com/ScavyXYZ/lwallpaper.

```cpp
#include <livewallpaper/livewallpaper.hpp>
livewallpaper::Wallpaper player;
auto error = player.play("C:/videos/space.mp4"); // waits for initialization
// ... player.isPlaying(), player.lastError() ...
player.stop();
```

The public header exposes no FFmpeg, SDL or Direct3D types. `play`, `stop`,
`state`, `isPlaying`, `setLogCallback`, `isSupported` and
`transcodeToScreenResolution` remain available; `lastError` exposes failures
that occur after startup.

Internals use the tested D3D11 shared-device decoder/renderer, bounded frame
queue, pooled software-frame buffers, original-frame PTS, late-frame dropping,
interruptible timing, and desktop-visibility pause. Import copies compatible
H.264 unchanged or encodes without upscaling using NVENC/QSV/AMF with software
fallback. Errors propagate after decoder shutdown; native windows use RAII.

## Build and test

```powershell
git lfs install
git lfs pull
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

No Qt installation is needed. Repeat with `Debug` for debug builds. Tests need
the FFmpeg CLI on PATH for media fixtures. Runtime DLLs are copied to
`build/bin/<configuration>/`. Unit, public API, media/decoder, and private GPU
tests never modify the desktop. `livewallpaper_selftest` is manual and DOES
change the desktop while running.

FFmpeg 9.0.2 (Gyan full shared, GPL-3.0) and official SDL 3.4.20 are pinned in
`third_party/versions.json`. DLLs/import libraries use Git LFS; review the
dependency licenses before distributing binaries. See `third_party/README.md`.

## GUI integration

Consumers pin this repository as a git submodule and use:

```cmake
set(LIVEWALLPAPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/lwallpaper_core)
target_link_libraries(my_gui PRIVATE livewallpaper::livewallpaper)
```

Consumers may supply `FFMPEG_ROOT` / `SDL3_ROOT`. Deploy the matching runtime
DLLs beside the executable. Only this repository implements playback/import;
the GUI owns collection management and atomic file publication.
