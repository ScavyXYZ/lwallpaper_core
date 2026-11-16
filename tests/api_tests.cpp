#include <livewallpaper/livewallpaper.hpp>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        livewallpaper::Wallpaper player;
        if (player.isPlaying()) throw std::runtime_error("New player must be idle.");
        player.stop();
        if (player.play("").empty()) throw std::runtime_error("Empty path must fail.");
        if (player.play("missing-lwallpaper-test-file.mp4").empty())
            throw std::runtime_error("Missing path must fail.");
        if (player.isPlaying()) throw std::runtime_error("Failed playback must leave the player idle.");
        player.stop();
        std::atomic<bool> cancelled{true};
        if (livewallpaper::transcodeToScreenResolution("", "", cancelled).empty())
            throw std::runtime_error("Cancelled import must fail.");
        std::cout << "PASS: public API, idle stop, invalid input, cancelled import; no desktop touched\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
