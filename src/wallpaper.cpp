#include <livewallpaper/livewallpaper.hpp>
#include "wallpaper/wallpaper_core.hpp"
#include "wallpaper/window_handle.hpp"
#include "render/d3d11_zero_copy.hpp"
#include <future>
#include <filesystem>

namespace livewallpaper {
namespace {
    // Allows stop() from a diagnostic callback without joining the calling thread.
    thread_local const void* renderingInstance = nullptr;
}

struct Wallpaper::Impl {
    explicit Impl(LogCallback sink) : log(std::move(sink)) {}
    ~Impl() { stop(); }

    void report(const std::string& text) noexcept {
        LogCallback sink;
        { std::lock_guard lock(logMutex); sink = log; }
        try {
            if (sink) sink(LogLevel::Error, text);
            else std::cerr << text << '\n';
        } catch (...) { /* A diagnostic sink cannot break playback cleanup. */ }
    }

    void stop() {
        cancelled = true;
        if (renderingInstance == this) return;
        std::lock_guard lock(lifecycle);
        cancelled = true;
        if (thread.joinable()) thread.join();
        state = State::Idle;
    }

    std::string play(const std::string& path) {
        if (renderingInstance == this) return "Cannot restart playback from a log callback.";
        std::lock_guard lock(lifecycle);
        cancelled = true;
        if (thread.joinable()) thread.join();
        state = State::Idle;
        { std::lock_guard errorLock(errorMutex); lastError.clear(); }
        if (path.empty() || !std::filesystem::exists(std::filesystem::u8path(path)))
            return "Video file not found: " + path;
        cancelled = false;
        std::promise<std::string> ready;
        auto result = ready.get_future();
        try {
            thread = std::thread([this, path, ready = std::move(ready)]() mutable {
                renderingInstance = this;
                bool started = false;
                auto run = [&]() {
                    const auto [width, height] = wallpaper::physical_screen_size();
                    HWND host = wallpaper::wallpaper_hwnd(width, height);
                    if (!host) throw std::runtime_error("Cannot locate the desktop wallpaper window.");
                    wallpaper::WindowHandle window(wallpaper::create_wallpaper_child(
                        GetModuleHandleW(nullptr), host, width, height));
                    FrameQueue queue(kFrameQueueCapacity);
                    wallpaper::D3D11ZeroCopyRenderer gpu;
                    const bool gpuReady = gpu.initialize(window.get(), width, height);
                    Decoder decoder(path, queue, cancelled);
                    if (gpuReady) decoder.external_d3d_device = gpu.device();
                    decoder.start();
                    auto signalReady = [&]() {
                        state = State::Playing;
                        started = true;
                        ready.set_value("");
                    };
                    if (gpuReady && decoder.is_zero_copy_active()) {
                        gpu.set_sync_callbacks(decoder.hw_lock, decoder.hw_unlock, decoder.hw_lock_ctx);
                        signalReady();
                        wallpaper::VisibilityGate gate(window.get());
                        wallpaper::PlaybackClock clock;
                        bool paused = false;
                        while (!cancelled) {
                            wallpaper::pump_win32_events(cancelled);
                            if (cancelled) break;
                            if (!gate.visible()) {
                                paused = true;
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                continue;
                            }
                            if (paused) { clock.resync(); paused = false; }
                            YUVFrame frame;
                            if (!queue.pop(frame, cancelled, decoder.done_flag())) break;
                            clock.on_frame(frame.pts_seconds);
                            if (clock.should_drop(frame.pts_seconds)) continue;
                            if (!clock.wait_for(frame.pts_seconds, cancelled)) break;
                            if (!frame.is_raw_hw_frame)
                                throw std::runtime_error("Unexpected CPU frame on the D3D11 path.");
                            if (!gpu.present_frame(frame.frame.get()) && !gpu.is_valid())
                                throw std::runtime_error(gpu.last_error());
                        }
                    } else {
                        // SDL is owned by this render thread only when the host has not initialized it.
                        struct SdlGuard {
                            bool owned = SDL_WasInit(SDL_INIT_VIDEO) == 0;
                            SdlGuard() {
                                if (owned && !SDL_Init(SDL_INIT_VIDEO))
                                    throw std::runtime_error(SDL_GetError());
                            }
                            ~SdlGuard() { if (owned) SDL_QuitSubSystem(SDL_INIT_VIDEO); }
                        } sdl;
                        Renderer renderer(window.get(), width, height);
                        signalReady();
                        renderer.run(queue, cancelled, decoder.done_flag(), &decoder.nv12_passthrough_ok);
                    }
                    cancelled = true;
                    queue.cancel();
                    decoder.join();
                    decoder.rethrow_error();
                };
                try { run(); }
                catch (const std::exception& error) {
                    { std::lock_guard lock(errorMutex); lastError = error.what(); }
                    if (!started) ready.set_value(error.what());
                    report(error.what());
                }
                catch (...) {
                    { std::lock_guard lock(errorMutex); lastError = "Unknown playback failure."; }
                    if (!started) ready.set_value("Unknown playback failure.");
                    report("Unknown playback failure.");
                }
                state = State::Idle;
                renderingInstance = nullptr;
            });
        } catch (const std::exception& error) { return error.what(); }
        const std::string error = result.get();
        if (!error.empty() && thread.joinable()) thread.join();
        return error;
    }

    std::mutex lifecycle, logMutex, errorMutex;
    std::string lastError;
    std::thread thread;
    std::atomic<bool> cancelled{false};
    std::atomic<State> state{State::Idle};
    LogCallback log;
};

Wallpaper::Wallpaper(LogCallback log) : impl_(std::make_unique<Impl>(std::move(log))) {}
Wallpaper::~Wallpaper() = default;
std::string Wallpaper::play(const std::string& path) { return impl_->play(path); }
void Wallpaper::stop() { impl_->stop(); }
State Wallpaper::state() const noexcept { return impl_->state.load(); }
std::string Wallpaper::lastError() const {
    std::lock_guard lock(impl_->errorMutex);
    return impl_->lastError;
}
void Wallpaper::setLogCallback(LogCallback log) {
    std::lock_guard lock(impl_->logMutex);
    impl_->log = std::move(log);
}
bool Wallpaper::isSupported() {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
}
std::string transcodeToScreenResolution(const std::string& input, const std::string& output,
                                      const std::atomic<bool>& cancelled) {
    try {
        wallpaper::transcode_video_to_screen(input, output, cancelled);
        return "";
    } catch (const std::exception& error) { return error.what(); }
}
}
