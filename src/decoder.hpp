#pragma once

#include "frame_queue.hpp"

#include <d3d11.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace livewallpaper::detail {

// Decodes a video file into a FrameQueue, looping forever at end of file.
//
// Hardware decode is attempted first (D3D11VA). When `externalD3dDevice` is
// supplied the decoder is initialised on that device, which is what allows the
// zero-copy render path to sample decoded surfaces directly; when the shared
// device cannot be used the decoder falls back to its own device and frames
// arrive as CPU-side planes instead.
class Decoder {
public:
    Decoder(std::string path, FrameQueue& queue, std::atomic<bool>& cancelled);
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Must be set before start() to share a device with the renderer.
    void setExternalD3DDevice(ID3D11Device* device) { externalDevice_ = device; }

    // Throws std::runtime_error on failure.
    void start();
    void join();

    // True once the decoder confirmed it produces raw D3D11 surfaces on the
    // shared device, i.e. the zero-copy render path is usable.
    bool isZeroCopyActive() const noexcept { return zeroCopyCapable_; }

    // FFmpeg's D3D11VA device lock, valid only while isZeroCopyActive(). The
    // renderer must hold it around any D3D11 call it makes, because it shares
    // the single immediate context with this decoder's thread.
    void (*hwLock)(void*) = nullptr;
    void (*hwUnlock)(void*) = nullptr;
    void*  hwLockCtx = nullptr;

    std::atomic<bool>& doneFlag() noexcept { return done_; }

    // Set by the renderer when it cannot display NV12 textures; the decoder then
    // converts to YUV420P on the CPU instead.
    std::atomic<bool> nv12PassthroughOk{ true };

private:
    void open();
    void run();
    void drainFrames(AVFrame* frame, int& frameCount);
    void seekToStart();
    void tryEnableHwDecode(const AVCodec* codec, ID3D11Device* sharedDevice);
    void buildSwscale();
    void forceNv12Conversion(const AVFrame* nv12Frame);
    void ensureConversionTarget(const AVFrame* transferred);
    VideoFrame buildFrame(AVFrame* src, AVFrame* original, bool isNv12);

    void releaseOpenResources() noexcept;

    std::string             path_;
    FrameQueue&             queue_;
    std::atomic<bool>&      cancelled_;
    std::atomic<bool>       done_{ false };
    std::thread             thread_;

    AVFormatContext*        fmtCtx_  = nullptr;
    AVCodecContext*         codecCtx_ = nullptr;
    AVBufferRef*            hwDeviceCtx_ = nullptr;
    AVBufferRef*            hwFramesCtx_ = nullptr;
    SwsContext*             swsCtx_  = nullptr;
    AVFrame*                yuvFrame_ = nullptr;
    uint8_t*                swsBuf_  = nullptr;
    std::size_t             swsBufSize_ = 0;

    int        streamIdx_ = -1;
    double     timeBase_ = 0.0;
    bool       needsConversion_ = false;
    bool       hwAccelActive_ = false;
    bool       zeroCopyCapable_ = false;
    bool       opened_ = false;
    AVPixelFormat swsSrcFmt_ = AV_PIX_FMT_NONE;
    int        swsSrcW_ = 0;
    int        swsSrcH_ = 0;
    AVPixelFormat hwPixFmt_ = AV_PIX_FMT_NONE;
    ID3D11Device* externalDevice_ = nullptr;

    // FFmpeg's get_format callback is a plain function pointer with no user data
    // slot, so the owning decoder travels through AVCodecContext::opaque.
    static AVPixelFormat getHwFormat(AVCodecContext* ctx, const AVPixelFormat* pixFmts);
};

} // namespace livewallpaper::detail
