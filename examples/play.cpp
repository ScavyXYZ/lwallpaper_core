// Minimal usage of the livewallpaper library.
//
//   livewallpaper_example <video> [--transcode <output>]
//
// Without --transcode the file is played as-is. With it, the file is first
// converted to a wallpaper-sized H.264 and the result is played instead, which
// is what you want for arbitrary footage.

#include <livewallpaper/livewallpaper.hpp>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <video> [--transcode <output>]\n", argv[0]);
        return 1;
    }

    const std::string video = argv[1];
    std::string playFile = video;

    if (argc >= 4 && std::string(argv[2]) == "--transcode") {
        playFile = argv[3];
        std::printf("Transcoding %s -> %s\n", video.c_str(), playFile.c_str());
        const std::string error =
            livewallpaper::transcodeToScreenResolution(video, playFile, std::atomic<bool>{ false });
        if (!error.empty()) {
            std::printf("Transcode failed: %s\n", error.c_str());
            return 1;
        }
    }

    if (!livewallpaper::Wallpaper::isSupported()) {
        std::printf("This machine cannot play video wallpapers.\n");
        return 1;
    }

    livewallpaper::Wallpaper wallpaper([](livewallpaper::LogLevel level, std::string_view msg) {
        const char* tag = level == livewallpaper::LogLevel::Error ? "error"
                         : level == livewallpaper::LogLevel::Warn  ? "warn"
                         : level == livewallpaper::LogLevel::Debug ? "debug"
                                                                   : "info";
        std::printf("[%s] %.*s\n", tag, static_cast<int>(msg.size()), msg.data());
    });

    const std::string error = wallpaper.play(playFile);
    if (!error.empty()) {
        std::printf("Could not start the wallpaper: %s\n", error.c_str());
        return 1;
    }

    std::printf("Playing %s. Press Enter to stop.\n", playFile.c_str());
    std::fflush(stdout);
    std::string line;
    std::getline(std::cin, line);

    wallpaper.stop();
    return 0;
}
