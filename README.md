# livewallpaper

Embed a looping video as the Windows desktop wallpaper.

A small C++17 static library. You get one header, one `.lib`, and three calls:

```cpp
#include <livewallpaper/livewallpaper.hpp>

livewallpaper::Wallpaper wallpaper;
if (const std::string error = wallpaper.play("C:/videos/space.webm"); !error.empty()) {
    // error is a human-readable message; nothing was left running
}
...
wallpaper.stop();
```

No ffmpeg, SDL or D3D11 types appear in the public header, so consumers never
see or have to include any of them.

## How it works

Frames are decoded with FFmpeg (hardware decode via D3D11VA when available) and
drawn straight to a swapchain on a child window of the desktop's `WorkerW`
window - the same trick Windows itself uses for animated wallpapers.

The primary path is zero-copy: the renderer owns the D3D11 device, the decoder is
initialised on that same device, and a small pixel shader samples the decoded
NV12 surface directly. Decoded pixels never touch system memory. If D3D11VA is
unavailable the library falls back automatically to uploading YUV/NV12 frames
through SDL, and to software decode if that is needed too.

The video loops until stopped. Frame timing follows each frame's presentation
timestamp, so playback is smooth and stays in sync with wall-clock time rather
than decoding as fast as it can.

## API

```cpp
namespace livewallpaper {

class Wallpaper {
    explicit Wallpaper(LogCallback log = {});
    ~Wallpaper();

    // Starts playback on a background thread. Returns "" on success, otherwise a
    // human-readable error (and nothing is left running). Calling play() while
    // already playing replaces the current video.
    std::string play(const std::string& videoPath);

    // Stops playback and blocks until the thread has finished and the wallpaper
    // window is gone. Safe from any thread, and safe when idle.
    void stop();

    State  state() const noexcept;
    bool   isPlaying() const noexcept;

    void setLogCallback(LogCallback log);
    static bool isSupported();
};

// Converts any video into a wallpaper-sized H.264 file. Returns "" on success.
// `cancelled` is polled and may be set from another thread.
std::string transcodeToScreenResolution(const std::string& inputPath,
                                       const std::string& outputPath,
                                       const std::atomic<bool>& cancelled);

}
```

`transcodeToScreenResolution` is a standalone helper: the library deliberately
does not manage a wallpaper collection or an index file, so you decide where
converted videos live.

## Building

Requires Windows, a 64-bit toolchain, CMake 3.21+ and MSVC.

Two dependencies are expected as plain "devel" trees rather than installed
packages:

- **ffmpeg** — a tree with `include/` and `lib/` (import libraries plus the
  matching runtime DLLs in `bin/`)
- **SDL3** — the Visual Studio package, a tree with `include/` and `lib/x64/`

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Both are auto-detected under `third_party/` or `lwallpaper/`. To point elsewhere:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DFFMPEG_ROOT=C:/deps/ffmpeg `
      -DSDL3_ROOT=C:/deps/SDL3-3.4.2-VC/SDL3-3.4.2
```

### Targets

| Target | What it is |
| --- | --- |
| `livewallpaper` | the static library |
| `livewallpaper_example` | plays a file given on the command line |
| `livewallpaper_selftest` | exercises stop / restart / error / transcode paths |

### Runtime DLLs

The library is static, but ffmpeg and SDL3 are not, so their DLLs must sit next
to your executable (or on `PATH`):

```
avcodec-62.dll  avdevice-62.dll  avfilter-11.dll  avformat-62.dll
avutil-60.dll   swresample-6.dll swscale-9.dll     SDL3.dll
```

## Using it from another project

Install the package and use `find_package`:

```powershell
cmake --install build --config Release --prefix C:/deps/livewallpaper
```

```cmake
find_package(livewallpaper REQUIRED)
target_link_libraries(my_app PRIVATE livewallpaper::livewallpaper)
```

Or vendor the sources directly - `add_subdirectory` works as-is, because
`FFMPEG_ROOT` and `SDL3_ROOT` can be set as cache variables beforehand.

## Examples

```powershell
# play as-is
build\Release\livewallpaper_example.exe C:\videos\space.webm

# convert first, then play
build\Release\livewallpaper_example.exe C:\videos\clip.mov --transcode C:\videos\wall.mp4
```

```powershell
# run the self test (needs a video and a writable scratch dir)
build\Release\livewallpaper_selftest.exe C:\videos\space.webm C:\temp
```

## Notes and limitations

- **Windows only.** The wallpaper is embedded in the shell's `WorkerW` window.
- **Single active wallpaper per process**, per `Wallpaper` object. Create
  several objects if you really want several.
- **`play()` blocks until start-up finishes** (window creation, device and
  decoder init) so that it can report errors. Playback itself is asynchronous.
- **Stopping is synchronous.** `stop()` returns only once the background thread
  has exited and the wallpaper window has been destroyed.
- `Wallpaper` is not thread-safe as an object, with one exception: `stop()` may
  be called from any thread (that is its purpose). Calls that race on the same
  object otherwise need your own synchronisation.
- Logging goes to `stderr` unless you pass a `LogCallback`. The callback is
  invoked from internal threads, so keep it cheap and thread-safe.
- Only the first video stream is used, and audio is ignored.
