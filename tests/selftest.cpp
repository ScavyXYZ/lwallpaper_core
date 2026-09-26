// Exercises the parts a screenshot cannot: clean stop, restarting on the same
// object, error reporting, and the standalone transcode helper.
//
//   livewallpaper_selftest <video> <scratch-dir>

#include <livewallpaper/livewallpaper.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-46s %s\n", what, ok ? "ok" : "FAILED");
    std::fflush(stdout);
    if (!ok) ++failures;
}

void step(const char* what) {
    std::printf("-> %s\n", what);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <video> <scratch-dir>\n", argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string video = argv[1];
    const std::string scratch = argv[2];

    livewallpaper::Wallpaper wallpaper([](livewallpaper::LogLevel lvl, std::string_view msg) {
        if (lvl == livewallpaper::LogLevel::Error || lvl == livewallpaper::LogLevel::Warn) {
            std::printf("  [%s] %.*s\n", lvl == livewallpaper::LogLevel::Error ? "error" : "warn",
                        static_cast<int>(msg.size()), msg.data());
        }
    });

    check(livewallpaper::Wallpaper::isSupported(), "isSupported()");

    check(wallpaper.play("").empty() == false, "play(\"\") reports an error");
    check(wallpaper.play("no-such-file-" "x.mp4").empty() == false, "play(missing file) reports an error");
    check(wallpaper.state() == livewallpaper::State::Idle, "state is Idle after a failed play");

    step("play #1");
    const std::string first = wallpaper.play(video);
    check(first.empty(), "play() starts");
    if (!first.empty()) std::printf("  play said: %s\n", first.c_str());
    check(wallpaper.isPlaying(), "isPlaying() after play");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    wallpaper.stop();
    check(wallpaper.state() == livewallpaper::State::Idle, "state is Idle after stop");
    check(!wallpaper.isPlaying(), "not playing after stop");

    step("play #2 (restart)");
    const std::string second = wallpaper.play(video);
    check(second.empty(), "play() restarts on the same object");
    if (!second.empty()) std::printf("  restart said: %s\n", second.c_str());
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Switching videos without stopping first must be safe.
    step("play #3 (switch while playing)");
    const std::string third = wallpaper.play(video);
    check(third.empty(), "play() while playing switches cleanly");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    wallpaper.stop();

    wallpaper.stop();
    check(true, "stop() when idle is harmless");

    step("transcode");
    const std::string out = scratch + "/selftest_out.mp4";
    std::remove(out.c_str());
    const std::string err = livewallpaper::transcodeToScreenResolution(video, out,
                                                                       std::atomic<bool>{ false });
    check(err.empty(), "transcodeToScreenResolution() succeeds");
    if (!err.empty()) std::printf("  transcode said: %s\n", err.c_str());

    std::atomic<bool> cancelled{ true };
    const std::string out2 = scratch + "/selftest_cancelled.mp4";
    const std::string err2 = livewallpaper::transcodeToScreenResolution(video, out2, cancelled);
    check(!err2.empty(), "transcode honours a pre-set cancel flag");

    std::printf("\n%s (%d failure(s))\n", failures ? "SELFTEST FAILED" : "SELFTEST PASSED", failures);
    return failures ? 1 : 0;
}
