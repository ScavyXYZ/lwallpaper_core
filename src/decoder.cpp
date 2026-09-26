#include "decoder.hpp"
#include "log.hpp"

#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/error.h>
}

namespace livewallpaper::detail {
namespace {

std::string avError(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

void avBufferUnref(AVBufferRef** p) {
    if (p && *p) av_buffer_unref(p);
}

} // namespace

Decoder::Decoder(std::string path, FrameQueue& queue, std::atomic<bool>& cancelled)
    : path_(std::move(path)), queue_(queue), cancelled_(cancelled) {
    LW_LOG_INFO("Decoder created for: " << path_);
}

Decoder::~Decoder() {
    join();
    releaseOpenResources();
}

void Decoder::releaseOpenResources() noexcept {
    if (yuvFrame_) { av_frame_free(&yuvFrame_); }
    if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
    if (swsBuf_) { av_free(swsBuf_); swsBuf_ = nullptr; swsBufSize_ = 0; }
    avBufferUnref(&hwFramesCtx_);
    avBufferUnref(&hwDeviceCtx_);
    if (codecCtx_) { avcodec_free_context(&codecCtx_); }
    if (fmtCtx_) { avformat_close_input(&fmtCtx_); }
    opened_ = false;
}

void Decoder::start() {
    open();
    opened_ = true;
    thread_ = std::thread(&Decoder::run, this);
}

void Decoder::join() {
    if (thread_.joinable()) thread_.join();
}

void Decoder::seekToStart() {
    const int ret = av_seek_frame(fmtCtx_, streamIdx_, 0, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        LW_LOG_ERROR("Failed to seek to start: " << avError(ret));
        cancelled_ = true;
        return;
    }
    avcodec_flush_buffers(codecCtx_);
}

AVPixelFormat Decoder::getHwFormat(AVCodecContext* ctx, const AVPixelFormat* pixFmts) {
    auto* self = static_cast<Decoder*>(ctx->opaque);
    for (const AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == self->hwPixFmt_) return *p;
    }
    LW_LOG_WARN("Decoder did not offer the hardware surface format, using software");
    return pixFmts[0];
}

void Decoder::tryEnableHwDecode(const AVCodec* codec, ID3D11Device* sharedDevice) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) {
            LW_LOG_INFO("No D3D11VA hardware config for " << codec->name << ", using software decode");
            return;
        }
        if (!(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) ||
            cfg->device_type != AV_HWDEVICE_TYPE_D3D11VA) {
            continue;
        }

        AVBufferRef* deviceCtx = nullptr;

        if (sharedDevice) {
            deviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!deviceCtx) {
                LW_LOG_WARN("av_hwdevice_ctx_alloc failed, falling back to a private D3D11VA device");
            } else {
                auto* hwDevCtx = reinterpret_cast<AVHWDeviceContext*>(deviceCtx->data);
                auto* d3d11Ctx = static_cast<AVD3D11VADeviceContext*>(hwDevCtx->hwctx);
                sharedDevice->AddRef();
                d3d11Ctx->device = sharedDevice;

                if (av_hwdevice_ctx_init(deviceCtx) < 0) {
                    LW_LOG_WARN("Sharing the renderer's D3D11 device failed, falling back to a private one");
                    avBufferUnref(&deviceCtx);
                } else {
                    hwLock = d3d11Ctx->lock;
                    hwUnlock = d3d11Ctx->unlock;
                    hwLockCtx = d3d11Ctx->lock_ctx;
                    LW_LOG_INFO("D3D11VA is sharing the renderer's device (zero-copy enabled)");
                }
            }
        }

        if (!deviceCtx) {
            const int err = av_hwdevice_ctx_create(&deviceCtx, AV_HWDEVICE_TYPE_D3D11VA,
                                                   nullptr, nullptr, 0);
            if (err < 0) {
                LW_LOG_WARN("D3D11VA device creation failed: " << avError(err)
                                                             << ", using software decode");
                return;
            }
        }

        hwDeviceCtx_ = deviceCtx;
        codecCtx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
        hwPixFmt_ = cfg->pix_fmt;
        codecCtx_->opaque = this;
        codecCtx_->get_format = &Decoder::getHwFormat;
        hwAccelActive_ = true;
        zeroCopyCapable_ = (sharedDevice != nullptr) && (codecCtx_->hw_device_ctx != nullptr);
        LW_LOG_INFO("D3D11VA hardware decode enabled for " << codec->name);
        return;
    }
}

void Decoder::open() {
    int err = avformat_open_input(&fmtCtx_, path_.c_str(), nullptr, nullptr);
    if (err < 0)
        throw std::runtime_error("Cannot open video '" + path_ + "': " + avError(err));

    err = avformat_find_stream_info(fmtCtx_, nullptr);
    if (err < 0)
        throw std::runtime_error("Cannot read stream info: " + avError(err));

    const AVCodec* codec = nullptr;
    streamIdx_ = av_find_best_stream(fmtCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (streamIdx_ < 0 || !codec)
        throw std::runtime_error("No video stream found in '" + path_ + "'");

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_)
        throw std::runtime_error("Cannot allocate codec context");

    avcodec_parameters_to_context(codecCtx_, fmtCtx_->streams[streamIdx_]->codecpar);
    codecCtx_->thread_count = 0;
    codecCtx_->thread_type = FF_THREAD_FRAME;

    tryEnableHwDecode(codec, externalDevice_);

    // The zero-copy path holds decoded surfaces longer than the CPU path does,
    // because nothing is copied out of them until the renderer has presented
    // and released each one. FFmpeg sizes its D3D11VA pool for the decoder's own
    // reference frames only, so without this the queue could starve the decoder
    // of free surfaces.
    if (zeroCopyCapable_) {
        codecCtx_->extra_hw_frames = static_cast<int>(kFrameQueueCapacity) + 2;
    }

    err = avcodec_open2(codecCtx_, codec, nullptr);
    if (err < 0)
        throw std::runtime_error("Cannot open codec: " + avError(err));

    timeBase_ = av_q2d(fmtCtx_->streams[streamIdx_]->time_base);
    LW_LOG_INFO("Codec opened: " << codecCtx_->width << "x" << codecCtx_->height
                                 << ", hwaccel=" << (hwAccelActive_ ? "yes" : "no")
                                 << ", timebase=" << timeBase_);

    if (hwAccelActive_) {
        // codecCtx_->pix_fmt is a hardware surface format here, which swscale
        // cannot consume. The real transferred format is only known once the
        // first frame arrives, so swscale setup is deferred.
        return;
    }

    needsConversion_ = (codecCtx_->pix_fmt != AV_PIX_FMT_YUV420P &&
                        codecCtx_->pix_fmt != AV_PIX_FMT_YUVJ420P);
    if (needsConversion_) buildSwscale();
}

void Decoder::buildSwscale() {
    const int w = codecCtx_->width, h = codecCtx_->height;
    swsCtx_ = sws_getContext(w, h, codecCtx_->pix_fmt, w, h, AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) throw std::runtime_error("Cannot create SwsContext");

    yuvFrame_ = av_frame_alloc();
    if (!yuvFrame_) throw std::runtime_error("av_frame_alloc failed");

    const int sz = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, w, h, 1);
    if (sz < 0) throw std::runtime_error("av_image_get_buffer_size failed");
    swsBufSize_ = static_cast<std::size_t>(sz);
    swsBuf_ = static_cast<uint8_t*>(av_malloc(swsBufSize_));
    if (!swsBuf_) throw std::runtime_error("av_malloc failed");

    yuvFrame_->format = AV_PIX_FMT_YUV420P;
    yuvFrame_->width = w;
    yuvFrame_->height = h;
    av_image_fill_arrays(yuvFrame_->data, yuvFrame_->linesize, swsBuf_,
                         AV_PIX_FMT_YUV420P, w, h, 1);
}

void Decoder::run() {
    LW_LOG_INFO("Decoder thread started");

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (!frame || !packet) {
        LW_LOG_ERROR("Cannot allocate ffmpeg frame/packet");
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        done_ = true;
        queue_.cancel();
        return;
    }

    int frameCount = 0;
    while (!cancelled_) {
        const int ret = av_read_frame(fmtCtx_, packet);
        if (ret == AVERROR_EOF) {
            seekToStart();
            continue;
        }
        if (ret < 0) {
            LW_LOG_WARN("av_read_frame error: " << avError(ret) << ", skipping");
            continue;
        }

        if (packet->stream_index == streamIdx_) {
            const int sendRet = avcodec_send_packet(codecCtx_, packet);
            if (sendRet == 0) {
                drainFrames(frame, frameCount);
            } else if (sendRet < 0) {
                LW_LOG_WARN("avcodec_send_packet error: " << avError(sendRet));
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);

    done_ = true;
    LW_LOG_INFO("Decoder thread finished after " << frameCount << " frames");
    queue_.cancel();
}

void Decoder::drainFrames(AVFrame* frame, int& frameCount) {
    while (!cancelled_) {
        const int ret = avcodec_receive_frame(codecCtx_, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            LW_LOG_WARN("avcodec_receive_frame error: " << avError(ret));
            break;
        }

        if (zeroCopyCapable_ && hwAccelActive_ && frame->format == hwPixFmt_) {
            VideoFrame vf(av_frame_alloc());
            av_frame_ref(vf.frame.get(), frame);
            vf.width = codecCtx_->width;
            vf.height = codecCtx_->height;
            vf.isRawHwFrame = true;
            int64_t pts = frame->pts;
            if (pts == AV_NOPTS_VALUE) pts = frame->best_effort_timestamp;
            vf.ptsSeconds = (pts != AV_NOPTS_VALUE) ? pts * timeBase_ : 0.0;

            if (!queue_.push(std::move(vf), cancelled_)) break;
            ++frameCount;
            continue;
        }

        // Hardware-decoded frames live in GPU memory as a D3D11 surface handle
        // and must be transferred to system memory before swscale or the SDL
        // texture upload can touch their pixels.
        AVFrame* decoded = frame;
        AVFrame* transferred = nullptr;
        if (hwAccelActive_ && frame->format == hwPixFmt_) {
            transferred = av_frame_alloc();
            if (av_hwframe_transfer_data(transferred, frame, 0) < 0) {
                LW_LOG_WARN("Hardware frame transfer failed, dropping frame");
                av_frame_free(&transferred);
                continue;
            }
            decoded = transferred;
            ensureConversionTarget(decoded);
        }

        const auto fmt = static_cast<AVPixelFormat>(decoded->format);
        const bool isNv12 = (fmt == AV_PIX_FMT_NV12);
        bool passThrough = isNv12 && nv12PassthroughOk.load();

        if (isNv12 && !passThrough && (needsConversion_ == false || !swsCtx_)) {
            forceNv12Conversion(decoded);
        }

        AVFrame* src = decoded;
        if (needsConversion_ && swsCtx_ && !passThrough) {
            sws_scale(swsCtx_, decoded->data, decoded->linesize, 0, decoded->height,
                      yuvFrame_->data, yuvFrame_->linesize);
            src = yuvFrame_;
        }

        VideoFrame vf = buildFrame(src, decoded, passThrough);
        if (transferred) av_frame_free(&transferred);

        if (!queue_.push(std::move(vf), cancelled_)) break;
        ++frameCount;
    }
}

void Decoder::forceNv12Conversion(const AVFrame* nv12Frame) {
    const int w = nv12Frame->width, h = nv12Frame->height;
    LW_LOG_INFO("NV12 passthrough rejected by the renderer, converting on the CPU");

    swsCtx_ = sws_getContext(w, h, AV_PIX_FMT_NV12, codecCtx_->width, codecCtx_->height,
                             AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) throw std::runtime_error("Cannot create SwsContext for NV12 conversion");
    needsConversion_ = true;
    swsSrcFmt_ = AV_PIX_FMT_NV12;
    swsSrcW_ = w;
    swsSrcH_ = h;

    if (yuvFrame_) av_frame_free(&yuvFrame_);
    yuvFrame_ = av_frame_alloc();
    if (!yuvFrame_) throw std::runtime_error("av_frame_alloc failed");

    yuvFrame_->format = AV_PIX_FMT_YUV420P;
    yuvFrame_->width = codecCtx_->width;
    yuvFrame_->height = codecCtx_->height;

    const int sz = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, codecCtx_->width,
                                            codecCtx_->height, 1);
    if (sz < 0) throw std::runtime_error("av_image_get_buffer_size failed");
    av_free(swsBuf_);
    swsBufSize_ = static_cast<std::size_t>(sz);
    swsBuf_ = static_cast<uint8_t*>(av_malloc(swsBufSize_));
    if (!swsBuf_) throw std::runtime_error("av_malloc failed");

    av_image_fill_arrays(yuvFrame_->data, yuvFrame_->linesize, swsBuf_, AV_PIX_FMT_YUV420P,
                         codecCtx_->width, codecCtx_->height, 1);
}

void Decoder::ensureConversionTarget(const AVFrame* transferred) {
    const auto fmt = static_cast<AVPixelFormat>(transferred->format);
    const int w = transferred->width;
    const int h = transferred->height;

    if (swsCtx_ && fmt == swsSrcFmt_ && w == swsSrcW_ && h == swsSrcH_) return;

    LW_LOG_INFO("Transferred frame format: " << av_get_pix_fmt_name(fmt) << " " << w << "x" << h);

    // NV12 is handed to the renderer as-is; it either uploads an NV12 texture or
    // converts the frame itself.
    needsConversion_ = (fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_YUVJ420P &&
                        fmt != AV_PIX_FMT_NV12);
    swsSrcFmt_ = fmt;
    swsSrcW_ = w;
    swsSrcH_ = h;
    if (!needsConversion_) {
        if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
        return;
    }

    // D3D11VA surfaces are commonly aligned and padded (1080 -> 1088), so the
    // swscale source must be sized from the transferred frame's own dimensions
    // rather than the codec's logical size, or the scale is corrupted.
    swsCtx_ = sws_getContext(w, h, fmt, codecCtx_->width, codecCtx_->height,
                             AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) throw std::runtime_error("Cannot create SwsContext for transferred frame");

    if (yuvFrame_) av_frame_free(&yuvFrame_);
    yuvFrame_ = av_frame_alloc();
    if (!yuvFrame_) throw std::runtime_error("av_frame_alloc failed");

    yuvFrame_->format = AV_PIX_FMT_YUV420P;
    yuvFrame_->width = codecCtx_->width;
    yuvFrame_->height = codecCtx_->height;

    const int sz = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, codecCtx_->width,
                                            codecCtx_->height, 1);
    if (sz < 0) throw std::runtime_error("av_image_get_buffer_size failed");
    av_free(swsBuf_);
    swsBufSize_ = static_cast<std::size_t>(sz);
    swsBuf_ = static_cast<uint8_t*>(av_malloc(swsBufSize_));
    if (!swsBuf_) throw std::runtime_error("av_malloc failed");

    av_image_fill_arrays(yuvFrame_->data, yuvFrame_->linesize, swsBuf_, AV_PIX_FMT_YUV420P,
                         codecCtx_->width, codecCtx_->height, 1);
}

VideoFrame Decoder::buildFrame(AVFrame* src, AVFrame* original, bool isNv12) {
    VideoFrame vf(av_frame_alloc());

    vf.width = codecCtx_->width;
    vf.height = codecCtx_->height;
    vf.isNv12 = isNv12;

    int64_t pts = original->pts;
    if (pts == AV_NOPTS_VALUE) pts = original->best_effort_timestamp;
    vf.ptsSeconds = (pts != AV_NOPTS_VALUE) ? pts * timeBase_ : 0.0;

    if (!needsConversion_) {
        av_frame_ref(vf.frame.get(), src);
    } else {
        vf.frame->format = AV_PIX_FMT_YUV420P;
        vf.frame->width = codecCtx_->width;
        vf.frame->height = codecCtx_->height;
        av_frame_get_buffer(vf.frame.get(), 32);
        av_frame_copy(vf.frame.get(), src);
    }
    return vf;
}

} // namespace livewallpaper::detail
