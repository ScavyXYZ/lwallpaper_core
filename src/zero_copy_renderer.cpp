#include "zero_copy_renderer.hpp"
#include "log.hpp"

#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace livewallpaper::detail {
namespace {

// BT.709 limited-range NV12 -> full-range RGB, matching what the bundled
// transcoder produces.
constexpr const char* kNv12ToRgbShader = R"(
Texture2D<float>  YPlane  : register(t0);
Texture2D<float2> UVPlane : register(t1);
SamplerState      Samp    : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 pos = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = float2(pos.x * 0.5 + 0.5, 1.0 - (pos.y * 0.5 + 0.5));
    return o;
}

float4 PSMain(VSOut input) : SV_TARGET {
    float y = YPlane.Sample(Samp, input.uv);
    float2 uv = UVPlane.Sample(Samp, input.uv) - float2(0.5, 0.5);

    y = (y - 16.0 / 255.0) * (255.0 / 219.0);
    float u = uv.x * (255.0 / 224.0);
    float v = uv.y * (255.0 / 224.0);

    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;

    return float4(saturate(r), saturate(g), saturate(b), 1.0);
}
)";

std::string hresultStr(const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed, hr=0x%08lx", what, static_cast<unsigned long>(hr));
    return buf;
}

} // namespace

bool ZeroCopyRenderer::initialize(HWND hwnd, int width, int height) {
    if (!createDevice()) return false;
    if (!createSwapChain(hwnd, width, height)) { destroy(); return false; }
    if (!createShaders()) { destroy(); return false; }
    if (!createSampler()) { destroy(); return false; }
    width_ = width;
    height_ = height;
    valid_ = true;
    return true;
}

void ZeroCopyRenderer::destroy() {
    valid_ = false;
    sampler_.Reset();
    pixelShader_.Reset();
    vertexShader_.Reset();
    backBufferRtv_.Reset();
    swapchain_.Reset();
    context_.Reset();
    device_.Reset();
    sliceCopy_.Reset();
}

bool ZeroCopyRenderer::createDevice() {
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    Microsoft::WRL::ComPtr<ID3D11Device> baseDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> baseContext;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                  levels, static_cast<UINT>(std::size(levels)),
                                  D3D11_SDK_VERSION, &baseDevice, &got, &baseContext);
    if (FAILED(hr)) {
        lastError_ = hresultStr("D3D11CreateDevice", hr);
        return false;
    }

    hr = baseDevice.As(&device_);
    if (FAILED(hr)) { lastError_ = "ID3D11Device1 query failed"; return false; }
    hr = baseContext.As(&context_);
    if (FAILED(hr)) { lastError_ = "ID3D11DeviceContext1 query failed"; return false; }
    return true;
}

bool ZeroCopyRenderer::createSwapChain(HWND hwnd, int width, int height) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) { lastError_ = "IDXGIDevice query failed"; return false; }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) { lastError_ = "GetAdapter failed"; return false; }

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        lastError_ = "IDXGIFactory2 query failed";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // The flip model is mandatory here, not a preference. Under the legacy
    // bitblt model (DXGI_SWAP_EFFECT_DISCARD, BufferCount 1) the swapchain is
    // never bound as this WorkerW-child window's DWM redirection surface: every
    // Present() returns S_OK while DWM keeps compositing whatever was already
    // there, i.e. the desktop's own wallpaper, with nothing in the log to hint
    // at the problem. FLIP_DISCARD hands the buffer to the compositor directly,
    // which is the only thing that works behind the desktop.
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    HRESULT hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &desc,
                                                nullptr, nullptr, &swapchain_);
    if (FAILED(hr)) {
        lastError_ = hresultStr("CreateSwapChainForHwnd", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        lastError_ = "swapchain GetBuffer failed";
        return false;
    }
    if (FAILED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferRtv_))) {
        lastError_ = "CreateRenderTargetView failed";
        return false;
    }
    return true;
}

bool ZeroCopyRenderer::createShaders() {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;

    HRESULT hr = D3DCompile(kNv12ToRgbShader, std::strlen(kNv12ToRgbShader), "nv12_to_rgb",
                            nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        lastError_ = hresultStr("vertex shader compile", hr);
        if (errBlob) lastError_ += std::string(": ") + static_cast<const char*>(errBlob->GetBufferPointer());
        return false;
    }

    errBlob.Reset();
    hr = D3DCompile(kNv12ToRgbShader, std::strlen(kNv12ToRgbShader), "nv12_to_rgb",
                    nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        lastError_ = hresultStr("pixel shader compile", hr);
        if (errBlob) lastError_ += std::string(": ") + static_cast<const char*>(errBlob->GetBufferPointer());
        return false;
    }

    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                           nullptr, &vertexShader_))) {
        lastError_ = "CreateVertexShader failed";
        return false;
    }
    if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                          nullptr, &pixelShader_))) {
        lastError_ = "CreatePixelShader failed";
        return false;
    }
    return true;
}

bool ZeroCopyRenderer::createSampler() {
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&desc, &sampler_))) {
        lastError_ = "CreateSamplerState failed";
        return false;
    }
    return true;
}

bool ZeroCopyRenderer::makePlaneSrv(ID3D11Texture2D* texture, DXGI_FORMAT format,
                                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& out) {
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MostDetailedMip = 0;
    desc.Texture2D.MipLevels = 1;

    const HRESULT hr = device_->CreateShaderResourceView(texture, &desc, &out);
    if (FAILED(hr)) {
        lastError_ = hresultStr("CreateShaderResourceView", hr);
        return false;
    }
    return true;
}

bool ZeroCopyRenderer::ensureSliceCopyTexture(const D3D11_TEXTURE2D_DESC& srcDesc, UINT height) {
    if (sliceCopy_ && sliceCopyW_ == srcDesc.Width && sliceCopyH_ == height &&
        sliceCopyFmt_ == srcDesc.Format) {
        return true;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = srcDesc.Width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = srcDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &tex);
    if (FAILED(hr)) {
        lastError_ = hresultStr("CreateTexture2D (slice copy target)", hr);
        return false;
    }

    sliceCopy_ = tex;
    sliceCopyW_ = srcDesc.Width;
    sliceCopyH_ = height;
    sliceCopyFmt_ = srcDesc.Format;
    return true;
}

bool ZeroCopyRenderer::presentFrame(const AVFrame* frame) {
    if (!valid_ || !frame || frame->format != AV_PIX_FMT_D3D11) return false;

    // A D3D11 device only ever has ONE immediate context, so the context used
    // here is the very same one ffmpeg's D3D11VA decoder submits decode work to
    // from the decoder thread. An ID3D11DeviceContext is not safe for concurrent
    // use, so every D3D11 call below has to be serialised against the decoder via
    // ffmpeg's hwaccel lock. Guarding only the CopySubresourceRegion is not
    // enough: the shader resource views, the Draw and the Present touch the same
    // context as well, and leaving them unguarded makes the very first Present
    // block forever against the decoder thread - the wallpaper then never
    // appears and the log simply stops mid-frame, with no error.
    struct LockGuard {
        void (*unlock)(void*);
        void* ctx;
        ~LockGuard() { if (unlock) unlock(ctx); }
    } guard{ hwUnlock_, hwLockCtx_ };
    if (hwLock_) hwLock_(hwLockCtx_);

    auto* arrayTexture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const auto slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
    if (!arrayTexture) return false;

    D3D11_TEXTURE2D_DESC arrayDesc{};
    arrayTexture->GetDesc(&arrayDesc);

    if (!loggedFormatOnce_) {
        LW_LOG_INFO("Zero-copy surface: format=" << static_cast<int>(arrayDesc.Format)
                                                << " " << arrayDesc.Width << "x" << arrayDesc.Height
                                                << " arraySize=" << arrayDesc.ArraySize
                                                << " coded=" << frame->width << "x" << frame->height);
        loggedFormatOnce_ = true;
    }

    // D3D11VA pads its surfaces out to macroblock boundaries, so a 1080p frame
    // actually lives in a 1920x1088 texture whose bottom 8 rows are padding
    // rather than picture. Copying (and therefore sampling) only the coded rows
    // keeps that padding out of the image; otherwise those rows get stretched
    // into the visible area and the picture is slightly too tall.
    const UINT codedHeight =
        (frame->height > 0 && static_cast<UINT>(frame->height) < arrayDesc.Height)
            ? static_cast<UINT>(frame->height)
            : arrayDesc.Height;

    if (!ensureSliceCopyTexture(arrayDesc, codedHeight)) return false;

    // The base D3D11_TEX2D_ARRAY_SRV has no PlaneSlice field - that only exists
    // on D3D11_TEX2D_ARRAY_SRV1, which needs ID3D11Device3 and a higher feature
    // level. Rather than require that, copy just this one array slice into a
    // single-layer texture (a GPU-to-GPU copy, far cheaper than the CPU transfer
    // this path exists to avoid) and build ordinary non-array Y/UV views against
    // that, where the format alone selects the plane.
    const UINT srcSubresource = D3D11CalcSubresource(0, slice, 1);
    D3D11_BOX srcBox{};
    srcBox.left = 0;
    srcBox.top = 0;
    srcBox.front = 0;
    srcBox.right = arrayDesc.Width;
    srcBox.bottom = codedHeight;
    srcBox.back = 1;
    context_->CopySubresourceRegion(sliceCopy_.Get(), 0, 0, 0, 0,
                                    arrayTexture, srcSubresource, &srcBox);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ySrv, uvSrv;
    if (!makePlaneSrv(sliceCopy_.Get(), DXGI_FORMAT_R8_UNORM, ySrv)) return false;
    if (!makePlaneSrv(sliceCopy_.Get(), DXGI_FORMAT_R8G8_UNORM, uvSrv)) return false;

    ID3D11ShaderResourceView* srvs[2] = { ySrv.Get(), uvSrv.Get() };
    context_->PSSetShaderResources(0, 2, srvs);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    ID3D11RenderTargetView* rtv = backBufferRtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->Draw(3, 0);

    // Unbind before presenting so the next frame's shader resource views are not
    // created on a texture that is still bound, which on some drivers stalls.
    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, nullSrvs);

    // SyncInterval 0: frame pacing is already handled by the pts-based wait in
    // the render loop, and waiting on a composition signal from a window that
    // sits behind the desktop risks stalling indefinitely.
    const HRESULT hr = swapchain_->Present(0, 0);
    if (FAILED(hr)) {
        lastError_ = hresultStr("Present", hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            valid_ = false;
        }
        return false;
    }
    return true;
}

} // namespace livewallpaper::detail
