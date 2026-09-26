#include "sdl_renderer.hpp"
#include "desktop_window.hpp"
#include "log.hpp"

#include <SDL3/SDL.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <stdexcept>
#include <thread>

namespace livewallpaper::detail {
namespace {

void destroyWindow(SDL_Window* w) { if (w) SDL_DestroyWindow(w); }
void destroyRenderer(SDL_Renderer* r) { if (r) SDL_DestroyRenderer(r); }
void destroyTexture(SDL_Texture* t) { if (t) SDL_DestroyTexture(t); }

} // namespace

struct SdlRenderer::Impl {
    Impl(HWND hwnd, int w, int h) : width(w), height(h) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, hwnd);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
        window.reset(SDL_CreateWindowWithProperties(props));
        SDL_DestroyProperties(props);
        if (!window)
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

        renderer.reset(SDL_CreateRenderer(window.get(), nullptr));
        if (!renderer)
            throw std::runtime_error(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());

        LW_LOG_INFO("SDL fallback renderer ready (" << w << "x" << h << ")");
    }

    SDL_Texture* createTexture(int w, int h, bool nv12, std::atomic<bool>& nv12PassthroughOk) {
        SDL_Texture* raw = SDL_CreateTexture(renderer.get(),
                                             nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV,
                                             SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!raw && nv12) {
            // This GPU or backend cannot stream NV12 textures. Tell the decoder
            // to convert to YUV420P on the CPU from now on, and retry in that
            // format instead.
            LW_LOG_WARN("NV12 texture creation failed (" << SDL_GetError()
                                                       << "), falling back to IYUV");
            nv12PassthroughOk.store(false);
            raw = SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_IYUV,
                                    SDL_TEXTUREACCESS_STREAMING, w, h);
        }
        if (!raw)
            throw std::runtime_error(std::string("SDL_CreateTexture failed: ") + SDL_GetError());
        return raw;
    }

    void uploadAndPresent(SDL_Texture* tex, const AVFrame* f, bool texIsNv12) {
        if (texIsNv12) {
            if (!f->data[0] || !f->data[1]) return;
            if (!SDL_UpdateNVTexture(tex, nullptr, f->data[0], f->linesize[0],
                                     f->data[1], f->linesize[1])) {
                LW_LOG_ERROR("SDL_UpdateNVTexture failed: " << SDL_GetError());
                return;
            }
        } else {
            if (!f->data[0] || !f->data[1] || !f->data[2]) return;
            if (!SDL_UpdateYUVTexture(tex, nullptr, f->data[0], f->linesize[0],
                                      f->data[1], f->linesize[1],
                                      f->data[2], f->linesize[2])) {
                LW_LOG_ERROR("SDL_UpdateYUVTexture failed: " << SDL_GetError());
                return;
            }
        }

        if (!SDL_RenderTexture(renderer.get(), tex, nullptr, nullptr)) {
            LW_LOG_ERROR("SDL_RenderTexture failed: " << SDL_GetError());
            return;
        }
        if (!SDL_RenderPresent(renderer.get()))
            LW_LOG_ERROR("SDL_RenderPresent failed: " << SDL_GetError());
    }

    static bool pumpEvents(std::atomic<bool>& cancelled) {
        if (!pumpMessages(cancelled)) return false;
        if (cancelled.load()) return false;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                cancelled = true;
                return false;
            }
        }
        return true;
    }

    int width;
    int height;
    std::unique_ptr<SDL_Window, decltype(&destroyWindow)>   window{ nullptr, &destroyWindow };
    std::unique_ptr<SDL_Renderer, decltype(&destroyRenderer)> renderer{ nullptr, &destroyRenderer };
};

SdlRenderer::SdlRenderer(HWND hwnd, int width, int height)
    : impl_(std::make_unique<Impl>(hwnd, width, height)) {}

SdlRenderer::~SdlRenderer() = default;

void SdlRenderer::run(FrameQueue& queue, std::atomic<bool>& cancelled,
                      std::atomic<bool>& decoderDone,
                      std::atomic<bool>& nv12PassthroughOk) {
    std::unique_ptr<SDL_Texture, decltype(&destroyTexture)> texture{ nullptr, &destroyTexture };
    bool textureIsNv12 = false;

    const double perfFreq = static_cast<double>(SDL_GetPerformanceFrequency());
    double ptsOrigin = -1.0;
    double wallOrigin = 0.0;
    double lastPts = 0.0;
    bool firstFrame = true;
    int frameCount = 0;

    while (!cancelled.load()) {
        if (!Impl::pumpEvents(cancelled)) break;

        VideoFrame frame;
        if (!queue.pop(frame, cancelled, decoderDone)) break;

        if (!firstFrame && frame.ptsSeconds < lastPts - 0.5) {
            ptsOrigin = frame.ptsSeconds;
            wallOrigin = static_cast<double>(SDL_GetPerformanceCounter()) / perfFreq;
        }
        lastPts = frame.ptsSeconds;

        // (Re)create the texture on the first frame, or if the pixel format
        // changed mid-stream because the decoder started falling back after
        // createTexture reported NV12 is unsupported.
        if (firstFrame || frame.isNv12 != textureIsNv12) {
            const bool requestedNv12 = frame.isNv12;
            texture.reset(impl_->createTexture(frame.width, frame.height, requestedNv12,
                                               nv12PassthroughOk));
            textureIsNv12 = requestedNv12 && nv12PassthroughOk.load();
        }

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
        } else if (waitSec < -0.01 && frameTime > 0.1) {
            LW_LOG_WARN("Frame late by " << -waitSec << " sec");
        }

        // A wallpaper does not need microsecond-accurate presentation, so yield
        // the CPU instead of spinning it at 100% on one core every frame.
        while ((static_cast<double>(SDL_GetPerformanceCounter()) / perfFreq - wallOrigin) < frameTime) {
            std::this_thread::yield();
        }

        // A frame produced before the decoder learned that the texture fell back
        // to IYUV (or vice versa) has a plane layout the texture does not
        // expect; skip it, the next frame will match.
        if (frame.isNv12 != textureIsNv12) {
            LW_LOG_WARN("Skipping frame during NV12/IYUV texture transition");
            continue;
        }

        impl_->uploadAndPresent(texture.get(), frame.frame.get(), textureIsNv12);
        ++frameCount;
    }

    LW_LOG_INFO("SDL fallback renderer finished after " << frameCount << " frames");
}

} // namespace livewallpaper::detail
