#include "desktop_window.hpp"
#include "log.hpp"

#include <livewallpaper/livewallpaper.hpp>

#include <cmath>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace livewallpaper {
namespace {

void freeFormatContext(AVFormatContext*& ctx) {
    if (!ctx) return;
    if (!(ctx->oformat->flags & AVFMT_NOFILE) && ctx->pb) avio_closep(&ctx->pb);
    avformat_free_context(ctx);
}

} // namespace

std::string transcodeToScreenResolution(const std::string& inputPath,
                                        const std::string& outputPath,
                                        const std::atomic<bool>& cancelled) {
    AVFormatContext* inFmt = nullptr;
    AVFormatContext* outFmt = nullptr;
    AVCodecContext* decCtx = nullptr;
    AVCodecContext* encCtx = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* inPacket = nullptr;
    AVPacket* outPacket = nullptr;
    AVFrame* decFrame = nullptr;
    AVFrame* encFrame = nullptr;
    std::string error;

    auto cleanup = [&] {
        av_packet_free(&inPacket);
        av_packet_free(&outPacket);
        av_frame_free(&decFrame);
        av_frame_free(&encFrame);
        if (sws) sws_freeContext(sws);
        avcodec_free_context(&decCtx);
        avcodec_free_context(&encCtx);
        if (inFmt) avformat_close_input(&inFmt);
        freeFormatContext(outFmt);
    };

    try {
        if (avformat_open_input(&inFmt, inputPath.c_str(), nullptr, nullptr) < 0)
            throw std::runtime_error("Failed to open the input file.");
        if (avformat_find_stream_info(inFmt, nullptr) < 0)
            throw std::runtime_error("Failed to retrieve stream information.");

        int videoStream = -1;
        for (unsigned i = 0; i < inFmt->nb_streams; ++i) {
            if (inFmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStream = static_cast<int>(i);
                break;
            }
        }
        if (videoStream < 0)
            throw std::runtime_error("No video stream found in the input file.");

        const AVCodec* decoder = avcodec_find_decoder(inFmt->streams[videoStream]->codecpar->codec_id);
        if (!decoder) throw std::runtime_error("No suitable decoder found.");

        decCtx = avcodec_alloc_context3(decoder);
        if (!decCtx ||
            avcodec_parameters_to_context(decCtx, inFmt->streams[videoStream]->codecpar) < 0 ||
            avcodec_open2(decCtx, decoder, nullptr) < 0) {
            throw std::runtime_error("Failed to initialize the decoder.");
        }

        const auto screen = detail::physicalScreenSize();

        if (avformat_alloc_output_context2(&outFmt, nullptr, "mp4", outputPath.c_str()) < 0)
            throw std::runtime_error("Failed to create the output context.");

        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!encoder) throw std::runtime_error("H.264 encoder not found.");

        AVStream* outStream = avformat_new_stream(outFmt, nullptr);
        if (!outStream) throw std::runtime_error("Failed to create the output stream.");

        encCtx = avcodec_alloc_context3(encoder);
        if (!encCtx) throw std::runtime_error("Failed to allocate the encoder context.");

        encCtx->height = screen.height;
        encCtx->width = screen.width;
        encCtx->sample_aspect_ratio = AVRational{ 1, 1 };
        encCtx->pix_fmt = AV_PIX_FMT_YUV420P;

        AVRational fps = inFmt->streams[videoStream]->avg_frame_rate;
        if (fps.num == 0 || fps.den == 0) fps = inFmt->streams[videoStream]->r_frame_rate;
        if (fps.num == 0 || fps.den == 0) fps = AVRational{ 30, 1 };

        encCtx->framerate = fps;
        encCtx->time_base = av_inv_q(fps);

        // Playback always restarts from the first frame, so frequent keyframes
        // are unnecessary. A large GOP with periodic keyframes keeps the file
        // much smaller without hurting looping.
        int gopSize = static_cast<int>(std::lround(av_q2d(fps) * 10.0));
        encCtx->gop_size = gopSize > 0 ? gopSize : 250;

        if (outFmt->oformat->flags & AVFMT_GLOBALHEADER)
            encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        // "fast" produces noticeably smaller files than "ultrafast" at the same
        // quality, and the result is re-read from disk on every loop iteration
        // for as long as the wallpaper runs. "zerolatency" is for live streaming
        // and only hurts compression when writing to a file.
        av_opt_set(encCtx->priv_data, "preset", "fast", 0);
        av_opt_set(encCtx->priv_data, "crf", "23", 0);

        if (avcodec_open2(encCtx, encoder, nullptr) < 0 ||
            avcodec_parameters_from_context(outStream->codecpar, encCtx) < 0) {
            throw std::runtime_error("Failed to open or configure the encoder.");
        }

        if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&outFmt->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0)
                throw std::runtime_error("Failed to create the output file for writing.");
        }
        if (avformat_write_header(outFmt, nullptr) < 0)
            throw std::runtime_error("Failed to write the output file header.");

        sws = sws_getContext(decCtx->width, decCtx->height, decCtx->pix_fmt,
                             encCtx->width, encCtx->height, encCtx->pix_fmt,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) throw std::runtime_error("Failed to initialize SwsContext.");

        inPacket = av_packet_alloc();
        outPacket = av_packet_alloc();
        decFrame = av_frame_alloc();
        encFrame = av_frame_alloc();
        if (!inPacket || !outPacket || !decFrame || !encFrame)
            throw std::runtime_error("Failed to allocate ffmpeg structures.");

        encFrame->format = encCtx->pix_fmt;
        encFrame->width = encCtx->width;
        encFrame->height = encCtx->height;
        if (av_frame_get_buffer(encFrame, 0) < 0)
            throw std::runtime_error("Failed to allocate the output frame buffer.");

        auto writePacket = [&] {
            av_packet_rescale_ts(outPacket, encCtx->time_base, outStream->time_base);
            outPacket->stream_index = outStream->index;
            av_interleaved_write_frame(outFmt, outPacket);
            av_packet_unref(outPacket);
        };

        int64_t ptsCounter = 0;
        while (av_read_frame(inFmt, inPacket) >= 0) {
            if (cancelled.load()) break;

            if (inPacket->stream_index == videoStream &&
                avcodec_send_packet(decCtx, inPacket) >= 0) {
                while (avcodec_receive_frame(decCtx, decFrame) >= 0) {
                    if (cancelled.load()) break;

                    av_frame_make_writable(encFrame);
                    sws_scale(sws, decFrame->data, decFrame->linesize, 0, decCtx->height,
                              encFrame->data, encFrame->linesize);
                    encFrame->pts = ptsCounter++;

                    if (avcodec_send_frame(encCtx, encFrame) >= 0) {
                        while (avcodec_receive_packet(encCtx, outPacket) >= 0)
                            writePacket();
                    }
                    av_frame_unref(decFrame);
                }
            }
            av_packet_unref(inPacket);
        }

        if (!cancelled.load()) {
            avcodec_send_frame(encCtx, nullptr);
            while (avcodec_receive_packet(encCtx, outPacket) >= 0)
                writePacket();
            av_write_trailer(outFmt);
        } else {
            error = "Transcoding was cancelled.";
        }
    } catch (const std::exception& ex) {
        error = ex.what();
    }

    cleanup();
    return error;
}

} // namespace livewallpaper
