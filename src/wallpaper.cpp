#include <livewallpaper/livewallpaper.hpp>

#include "decoder.hpp"
#include "desktop_window.hpp"
#include "frame_queue.hpp"
#include "log.hpp"
#include "sdl_renderer.hpp"
#include "zero_copy_renderer.hpp"

#include <SDL3/SDL.h>
#include <d3d11.h>
#include <timeapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <exception>
#include <future>
#include <stdexcept>
#include <thread>

#pragma comment(lib, "winmm.lib")

namespace livewallpaper {
namespace {

// Releases SDL_Init only when this library was the one that initialised it, so
// an application that already uses SDL is left alone.
struct SdlVideoGuard {
    bool owned = false;

    SdlVideoGuard() {
        if (SDL_WasInit(SDL_INIT_VIDEO)) return;
        if (SDL_Init(SDL_INIT_VIDEO)) {
            owned = true;
        } else {
            LW_LOG_ERROR(std::string("SDL_Init failed: ") + SDL_GetError());
        }
    }
    ~SdlVideoGuard() { if (owned) SDL_Quit(); }
    SdlVideoGuard(const SdlVideoGuard&) = delete;
    SdlVideoGuard& operator=(const SdlVideoGuard&) = delete;
};

} // namespace

struct Wallpaper::Impl {
    // Releases play() once start-up has either succeeded or failed. Fire-and-forget
    // from a failure path, and keep the instance alive as a safety net so an
    // unexpected exit can never leave play() blocked forever.
    struct ReadySignal {
        std::shared_ptr<std::promise<void>> promise;
        std::atomic<bool> fired{ false };
        void fire() {
            if (fired.exchange(true)) return;
            try { promise->set_value(); } catch (...) {}
        }
    };

    explicit Impl(LogCallback log) { detail::setLogCallback(std::move(log)); }
    ~Impl() { stop(); }

    std::string start(const std::string& videoPath);
    void stop();

    void runThread(const std::string& videoPath, const std::shared_ptr<ReadySignal>& ready);
    void renderZeroCopy(detail::ZeroCopyRenderer& renderer, detail::FrameQueue& queue,
                        detail::Decoder& decoder);
    void teardown();

    std::thread        thread_;
    std::thread::id    renderThreadId_{};
    std::atomic<bool>  cancelled_{ false };
    std::atomic<State> state_{ State::Idle };

    // Written by the render thread before ReadySignal::fire(), read by the
    // caller afterwards; the promise provides the synchronisation.
    std::string startError_;

    // Owned for the duration of one playback session.
    HWND                              childWindow_ = nullptr;
    std::unique_ptr<detail::Decoder>  decoder_;
    detail::FrameQueue*               queue_ = nullptr;
};

std::string Wallpaper::Impl::start(const std::string& videoPath) {
    stop();

    if (videoPath.empty()) return "No video path given.";

    state_ = State::Playing;
    startError_.clear();

    auto ready = std::make_shared<ReadySignal>();
    ready->promise = std::make_shared<std::promise<void>>();
    auto future = ready->promise->get_future();

    cancelled_.store(false);
    thread_ = std::thread([this, videoPath, ready] { runThread(videoPath, ready); });
    future.wait();

    const std::string error = startError_;
    if (!error.empty()) {
        if (thread_.joinable()) thread_.join();
        state_ = State::Idle;
    }
    return error;
}

void Wallpaper::Impl::runThread(const std::string& videoPath,
                                const std::shared_ptr<ReadySignal>& ready) {
    renderThreadId_ = std::this_thread::get_id();

    // Safety net: if any path below returns without firing, play() is still
    // released (and sees whatever startError_ holds).
    struct Net {
        const std::shared_ptr<ReadySignal>& signal;
        ~Net() { signal->fire(); }
    } net{ ready };

    auto fail = [this, &ready](const std::string& message) {
        LW_LOG_ERROR(message);
        startError_ = message;
        ready->fire();
        teardown();
    };
    auto succeed = [this, &ready] {
        startError_.clear();
        ready->fire();
    };

    try {
        timeBeginPeriod(1);

        const auto screen = detail::physicalScreenSize();
        LW_LOG_INFO("Screen size " << screen.width << "x" << screen.height);

        HWND host = detail::findWallpaperHost(screen.width, screen.height);
        if (!host) {
            fail("Cannot locate the desktop wallpaper window.");
            return;
        }

        childWindow_ = detail::createWallpaperChild(host, screen.width, screen.height);
        if (!childWindow_) {
            fail("Cannot create the wallpaper window.");
            return;
        }

        // The zero-copy renderer owns the D3D11 device the decoder will decode
        // into, so it has to exist before the decoder starts.
        detail::ZeroCopyRenderer zeroCopy;
        const bool zeroCopyReady = zeroCopy.initialize(childWindow_, screen.width, screen.height);
        if (!zeroCopyReady) {
            LW_LOG_WARN("Zero-copy D3D11 path unavailable (" << zeroCopy.lastError()
                                                          << "), using the SDL fallback");
        }

        queue_ = new detail::FrameQueue(detail::kFrameQueueCapacity);
        decoder_ = std::make_unique<detail::Decoder>(videoPath, *queue_, cancelled_);
        if (zeroCopyReady) decoder_->setExternalD3DDevice(zeroCopy.device());
        decoder_->start();

        if (zeroCopyReady && decoder_->isZeroCopyActive()) {
            LW_LOG_INFO("Playing via the zero-copy D3D11 path");
            zeroCopy.setSyncCallbacks(decoder_->hwLock, decoder_->hwUnlock, decoder_->hwLockCtx);
            succeed();
            renderZeroCopy(zeroCopy, *queue_, *decoder_);
        } else {
            SdlVideoGuard sdlGuard;
            if (!SDL_WasInit(SDL_INIT_VIDEO)) {
                fail(std::string("Cannot initialise SDL for the fallback path: ") + SDL_GetError());
                return;
            }
            succeed();

            LW_LOG_INFO("Playing via the SDL fallback path");
            detail::SdlRenderer sdl(childWindow_, screen.width, screen.height);
            sdl.run(*queue_, cancelled_, decoder_->doneFlag(), decoder_->nv12PassthroughOk);
        }
    } catch (const std::exception& ex) {
        fail(ex.what());
        return;
    } catch (...) {
        fail("Unknown error while starting playback.");
        return;
    }

    teardown();
}

void Wallpaper::Impl::renderZeroCopy(detail::ZeroCopyRenderer& renderer,
                                     detail::FrameQueue& queue, detail::Decoder& decoder) {
    const double perfFreq = static_cast<double>(SDL_GetPerformanceFrequency());
    double ptsOrigin = -1.0;
    double wallOrigin = 0.0;
    double lastPts = 0.0;
    bool firstFrame = true;
    int frameCount = 0;
    int presentFailures = 0;

    while (!cancelled_.load()) {
        if (!detail::pumpMessages(cancelled_)) break;

        detail::VideoFrame frame;
        if (!queue.pop(frame, cancelled_, decoder.doneFlag())) break;

        if (!firstFrame && frame.ptsSeconds < lastPts - 0.5) {
            ptsOrigin = frame.ptsSeconds;
            wallOrigin = static_cast<double>(SDL_GetPerformanceCounter()) / perfFreq;
        }
        lastPts = frame.ptsSeconds;

        if (firstFrame) {
            ptsOrigin = frame.ptsSeconds;
            wallOrigin = static_cast<double>(SDL_GetPerformanceCounter()) / perfFreq;
            firstFrame = false;
        }

        const double elapsed = static_cast<double>(SDL_GetPerformanceCounter()) / perfFreq - wallOrigin;
        const double frameTime = frame.ptsSeconds - ptsOrigin;
        const double waitSec = frameTime - elapsed;

        if (waitSec > 0.002) {
            SDL_Delay(static_cast<Uint32>((waitSec - 0.002) * 1000.0));
        }
        while ((static_cast<double>(SDL_GetPerformanceCounter()) / perfFreq - wallOrigin) < frameTime) {
            std::this_thread::yield();
        }

        if (!frame.isRawHwFrame) {
            // The decoder only takes this path once it has confirmed zero-copy is
            // active, so a CPU frame here would be misread as a D3D11 surface.
            LW_LOG_WARN("Unexpected non-hardware frame on the zero-copy path, skipping");
            continue;
        }

        if (!renderer.presentFrame(frame.frame.get())) {
            if (!renderer.isValid()) {
                LW_LOG_ERROR("Zero-copy renderer lost (" << renderer.lastError() << "), stopping");
                break;
            }
            if (presentFailures < 5)
                LW_LOG_WARN("presentFrame failed (" << renderer.lastError() << ")");
            ++presentFailures;
        }

        ++frameCount;
        if (frameCount % 600 == 0) {
            LW_LOG_INFO("Zero-copy: presented " << frameCount << " frames, "
                                                << presentFailures << " failures");
        }
    }

    LW_LOG_INFO("Zero-copy render loop finished after " << frameCount << " frames");
}

void Wallpaper::Impl::teardown() {
    cancelled_.store(true);
    if (queue_) queue_->cancel();
    if (decoder_) decoder_->join();

    delete queue_;
    queue_ = nullptr;
    decoder_.reset();

    if (childWindow_) {
        DestroyWindow(childWindow_);
        childWindow_ = nullptr;
    }
    timeEndPeriod(1);
    state_ = State::Idle;
}

void Wallpaper::Impl::stop() {
    cancelled_.store(true);
    if (queue_) queue_->cancel();

    // Joining from inside the render thread (e.g. a log callback that calls
    // stop) would deadlock, so just flag the cancellation there.
    if (thread_.joinable() && std::this_thread::get_id() != renderThreadId_)
        thread_.join();
}

Wallpaper::Wallpaper(LogCallback log) : impl_(std::make_unique<Impl>(std::move(log))) {}

Wallpaper::~Wallpaper() = default;

std::string Wallpaper::play(const std::string& videoPath) {
    return impl_->start(videoPath);
}

void Wallpaper::stop() {
    impl_->stop();
}

State Wallpaper::state() const noexcept {
    return impl_->state_.load();
}

void Wallpaper::setLogCallback(LogCallback log) {
    detail::setLogCallback(std::move(log));
}

bool Wallpaper::isSupported() {
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                         levels, 1, D3D11_SDK_VERSION,
                                         &device, &got, &context);
    return SUCCEEDED(hr);
}

} // namespace livewallpaper
