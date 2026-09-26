#pragma once

#include "frame_queue.hpp"

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace livewallpaper::detail {

// Fallback render path for machines where the zero-copy D3D11 route is
// unavailable: frames arrive as ordinary CPU-side YUV420P / NV12 planes and get
// uploaded into an SDL texture each frame.
class SdlRenderer {
public:
    SdlRenderer(HWND hwnd, int width, int height);
    ~SdlRenderer();

    SdlRenderer(const SdlRenderer&) = delete;
    SdlRenderer& operator=(const SdlRenderer&) = delete;

    void run(FrameQueue& queue, std::atomic<bool>& cancelled,
             std::atomic<bool>& decoderDone, std::atomic<bool>& nv12PassthroughOk);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace livewallpaper::detail
