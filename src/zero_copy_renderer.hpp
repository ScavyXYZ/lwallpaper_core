#pragma once

#include <d3d11_1.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

extern "C" {
#include <libavutil/frame.h>
}

namespace livewallpaper::detail {

// Renders D3D11VA-decoded frames straight to a swapchain without ever copying
// pixels through system memory.
//
// The device is created and owned here rather than by SDL, because the same
// device has to be shared with ffmpeg's hwaccel for the decode output to be
// sampleable without a copy. Any initialisation step can fail (old GPU, driver
// quirk, shader compile error); on failure the object is inert and every method
// is a safe no-op so the caller can fall back to the CPU path.
class ZeroCopyRenderer {
public:
    ZeroCopyRenderer() = default;
    ~ZeroCopyRenderer() { destroy(); }

    ZeroCopyRenderer(const ZeroCopyRenderer&) = delete;
    ZeroCopyRenderer& operator=(const ZeroCopyRenderer&) = delete;

    bool initialize(HWND hwnd, int width, int height);
    void destroy();

    bool isValid() const noexcept { return valid_; }
    const std::string& lastError() const noexcept { return lastError_; }

    // The device ffmpeg's D3D11VA hwaccel should be initialised on. Must be set
    // before the decoder is created.
    ID3D11Device* device() const noexcept { return device_.Get(); }

    // ffmpeg's D3D11VA lock pair. The renderer and the decoder share a single
    // immediate context, so every D3D11 call made while presenting has to be
    // serialised against the decoder thread through this lock.
    void setSyncCallbacks(void (*lock)(void*), void (*unlock)(void*), void* ctx) {
        hwLock_ = lock;
        hwUnlock_ = unlock;
        hwLockCtx_ = ctx;
    }

    // Presents one decoded frame. Returns false for a frame that could not be
    // drawn; that is not fatal, the caller should simply try the next frame.
    bool presentFrame(const AVFrame* frame);

private:
    bool createDevice();
    bool createSwapChain(HWND hwnd, int width, int height);
    bool createShaders();
    bool createSampler();
    bool makePlaneSrv(ID3D11Texture2D* texture, DXGI_FORMAT format,
                      Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& out);
    bool ensureSliceCopyTexture(const D3D11_TEXTURE2D_DESC& srcDesc, UINT height);

    Microsoft::WRL::ComPtr<ID3D11Device1>           device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1>    context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>         swapchain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  backBufferRtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         sliceCopy_;
    UINT       sliceCopyW_ = 0;
    UINT       sliceCopyH_ = 0;
    DXGI_FORMAT sliceCopyFmt_ = DXGI_FORMAT_UNKNOWN;

    void (*hwLock_)(void*) = nullptr;
    void (*hwUnlock_)(void*) = nullptr;
    void* hwLockCtx_ = nullptr;

    int  width_ = 0;
    int  height_ = 0;
    bool valid_ = false;
    bool loggedFormatOnce_ = false;
    std::string lastError_;
};

} // namespace livewallpaper::detail
