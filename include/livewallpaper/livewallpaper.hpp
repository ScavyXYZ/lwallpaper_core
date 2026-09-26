// livewallpaper - embed a looping video as the Windows desktop wallpaper.
//
// Public API. Deliberately free of ffmpeg / SDL / D3D11 types so that
// consumers only ever need this one header and the static library.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace livewallpaper {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
};

enum class State {
    Idle,
    Playing,
};

// Invoked from an internal thread for every diagnostic message. Optional - if
// no callback is set, messages are written to stderr (Info and above).
using LogCallback = std::function<void(LogLevel, std::string_view)>;

// Converts an arbitrary video into a wallpaper-sized H.264 file.
// Returns an empty string on success, otherwise a human-readable error.
// `cancelled` is polled periodically and may be set from another thread.
std::string transcodeToScreenResolution(const std::string& inputPath,
                                       const std::string& outputPath,
                                       const std::atomic<bool>& cancelled);

class Wallpaper {
public:
    // `log` receives every diagnostic message and, when set, suppresses the
    // default stderr output.
    explicit Wallpaper(LogCallback log = {});
    ~Wallpaper();

    Wallpaper(const Wallpaper&) = delete;
    Wallpaper& operator=(const Wallpaper&) = delete;

    // Starts playback of `videoPath` on a background thread and returns
    // immediately. An empty return value means playback started; anything else
    // is a human-readable error and nothing is left running.
    //
    // Calling play() while already playing stops the previous video first.
    std::string play(const std::string& videoPath);

    // Stops playback and blocks until the background thread has finished and
    // the wallpaper window has been torn down. Safe to call when idle, and
    // safe to call from any thread.
    void stop();

    State  state() const noexcept;
    bool   isPlaying() const noexcept { return state() == State::Playing; }

    // Replaces the log sink. May be called at any time, including while
    // playing.
    void setLogCallback(LogCallback log);

    // False when this build cannot play wallpapers at all (no usable GPU
    // decode and no usable fallback). play() will fail with an explanatory
    // message in that case.
    static bool isSupported();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace livewallpaper
