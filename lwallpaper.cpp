extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}
#include <SDL3/SDL.h>
#include <windows.h>
#include <shellscalingapi.h>

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shcore.lib")


namespace ffmpeg {
    struct FormatContextDeleter {
        void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
    };
    using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

    struct CodecContextDeleter {
        void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
    };
    using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

    struct FrameDeleter {
        void operator()(AVFrame* p) const { av_frame_free(&p); }
    };
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

    struct PacketDeleter {
        void operator()(AVPacket* p) const { av_packet_free(&p); }
    };
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

    struct SwsContextDeleter {
        void operator()(SwsContext* p) const { sws_freeContext(p); }
    };
    using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

    struct AvBuffer {
        uint8_t* data = nullptr;
        explicit AvBuffer(int size) : data(static_cast<uint8_t*>(av_malloc(size))) {}
        ~AvBuffer() { av_free(data); }
        AvBuffer(const AvBuffer&) = delete;
        AvBuffer& operator=(const AvBuffer&) = delete;
    };

    std::string av_error(int errnum) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(errnum, buf, sizeof(buf));
        return buf;
    }

}


struct YUVFrame {
    std::vector<uint8_t> y, u, v;
    int linesize_y{}, linesize_u{}, linesize_v{};
    int width{}, height{};
    double pts_seconds{};
};


class FrameQueue {
public:
    explicit FrameQueue(std::size_t capacity) : capacity_(capacity) {}

    bool push(YUVFrame frame, const std::atomic<bool>& cancelled) {
        std::unique_lock lock(mutex_);
        cv_push_.wait(lock, [&] {
            return queue_.size() < capacity_ || cancelled.load();
            });
        if (cancelled) return false;
        queue_.push(std::move(frame));
        cv_pop_.notify_one();
        return true;
    }

    bool pop(YUVFrame& out, const std::atomic<bool>& cancelled,
        const std::atomic<bool>& producer_done) {
        std::unique_lock lock(mutex_);
        cv_pop_.wait(lock, [&] {
            return !queue_.empty() || producer_done.load() || cancelled.load();
            });
        if (cancelled || (queue_.empty() && producer_done)) return false;
        out = std::move(queue_.front());
        queue_.pop();
        cv_push_.notify_one();
        return true;
    }

    void cancel() {
        cv_push_.notify_all();
        cv_pop_.notify_all();
    }

    void clear() {
        std::lock_guard lock(mutex_);
        while (!queue_.empty()) queue_.pop();
    }

private:
    std::size_t            capacity_;
    std::queue<YUVFrame>   queue_;
    std::mutex             mutex_;
    std::condition_variable cv_push_, cv_pop_;
};


class Decoder {
public:
    Decoder(const std::string& path, FrameQueue& queue, std::atomic<bool>& cancelled)
        : path_(path), queue_(queue), cancelled_(cancelled) {
    }

    void start() {
        open();
        thread_ = std::thread(&Decoder::run, this);
    }

    void join() {
        if (thread_.joinable()) thread_.join();
    }

    bool done() const { return done_.load(); }

    std::atomic<bool>& done_flag() { return done_; }

private:

    void open() {
        AVFormatContext* raw = nullptr;
        int err = avformat_open_input(&raw, path_.c_str(), nullptr, nullptr);
        if (err < 0)
            throw std::runtime_error("Cannot open video '" + path_ + "': " + ffmpeg::av_error(err));
        fmt_ctx_.reset(raw);

        err = avformat_find_stream_info(fmt_ctx_.get(), nullptr);
        if (err < 0)
            throw std::runtime_error("Cannot read stream info: " + ffmpeg::av_error(err));

        const AVCodec* codec = nullptr;
        stream_idx_ = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (stream_idx_ < 0)
            throw std::runtime_error("No video stream found.");

        AVCodecContext* raw_cc = avcodec_alloc_context3(codec);
        if (!raw_cc)
            throw std::runtime_error("Cannot allocate codec context.");
        codec_ctx_.reset(raw_cc);

        avcodec_parameters_to_context(codec_ctx_.get(),
            fmt_ctx_->streams[stream_idx_]->codecpar);
        codec_ctx_->thread_count = 0;
        codec_ctx_->thread_type = FF_THREAD_FRAME;

        err = avcodec_open2(codec_ctx_.get(), codec, nullptr);
        if (err < 0)
            throw std::runtime_error("Cannot open codec: " + ffmpeg::av_error(err));

        time_base_ = av_q2d(fmt_ctx_->streams[stream_idx_]->time_base);

        needs_conversion_ = (codec_ctx_->pix_fmt != AV_PIX_FMT_YUV420P &&
            codec_ctx_->pix_fmt != AV_PIX_FMT_YUVJ420P);
        if (needs_conversion_)
            build_sws();
    }

    void build_sws() {
        int w = codec_ctx_->width, h = codec_ctx_->height;
        SwsContext* raw_sws = sws_getContext(w, h, codec_ctx_->pix_fmt,
            w, h, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!raw_sws)
            throw std::runtime_error("Cannot create SwsContext.");
        sws_ctx_.reset(raw_sws);

        yuv_frame_.reset(av_frame_alloc());
        int sz = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, codec_ctx_->width,
            codec_ctx_->height, 1);
        sws_buf_ = std::make_unique<ffmpeg::AvBuffer>(sz);
        av_image_fill_arrays(yuv_frame_->data, yuv_frame_->linesize,
            sws_buf_->data, AV_PIX_FMT_YUV420P,
            codec_ctx_->width, codec_ctx_->height, 1);
    }


    void run() {
        auto frame = ffmpeg::FramePtr(av_frame_alloc());
        auto packet = ffmpeg::PacketPtr(av_packet_alloc());

        while (!cancelled_) {
            int ret = av_read_frame(fmt_ctx_.get(), packet.get());

            if (ret == AVERROR_EOF) {
                avcodec_send_packet(codec_ctx_.get(), nullptr);
                drain_frames(frame.get());
                break;
            }

            if (ret < 0) continue;

            if (packet->stream_index == stream_idx_) {
                ret = avcodec_send_packet(codec_ctx_.get(), packet.get());
                if (ret == 0)
                    drain_frames(frame.get());
            }
            av_packet_unref(packet.get());
        }

        done_ = true;
        queue_.cancel();
    }

    void drain_frames(AVFrame* frame) {
        while (!cancelled_) {
            int ret = avcodec_receive_frame(codec_ctx_.get(), frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            AVFrame* src = frame;
            if (needs_conversion_ && sws_ctx_) {
                sws_scale(sws_ctx_.get(), frame->data, frame->linesize,
                    0, codec_ctx_->height,
                    yuv_frame_->data, yuv_frame_->linesize);
                src = yuv_frame_.get();
            }

            YUVFrame yf = build_yuv_frame(src, frame);
            if (!queue_.push(std::move(yf), cancelled_)) break;
        }
    }

    YUVFrame build_yuv_frame(AVFrame* src, AVFrame* original) const {
        YUVFrame yf;
        yf.width = codec_ctx_->width;
        yf.height = codec_ctx_->height;
        yf.linesize_y = src->linesize[0];
        yf.linesize_u = src->linesize[1];
        yf.linesize_v = src->linesize[2];

        int64_t pts_raw = original->pts;
        if (pts_raw == AV_NOPTS_VALUE) pts_raw = original->best_effort_timestamp;
        yf.pts_seconds = (pts_raw != AV_NOPTS_VALUE) ? pts_raw * time_base_ : 0.0;

        int h = codec_ctx_->height;
        yf.y.assign(src->data[0], src->data[0] + src->linesize[0] * h);
        yf.u.assign(src->data[1], src->data[1] + src->linesize[1] * (h / 2));
        yf.v.assign(src->data[2], src->data[2] + src->linesize[2] * (h / 2));
        return yf;
    }


    std::string             path_;
    FrameQueue& queue_;
    std::atomic<bool>& cancelled_;
    std::atomic<bool>       done_{ false };
    std::thread             thread_;

    ffmpeg::FormatContextPtr fmt_ctx_;
    ffmpeg::CodecContextPtr  codec_ctx_;
    ffmpeg::SwsContextPtr    sws_ctx_;
    ffmpeg::FramePtr         yuv_frame_;
    std::unique_ptr<ffmpeg::AvBuffer> sws_buf_;

    int    stream_idx_ = -1;
    double time_base_ = 0.0;
    bool   needs_conversion_ = false;
};


namespace wallpaper {

    struct ScreenSize { int w, h; };

    ScreenSize physical_screen_size() {
        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm))
            return { static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight) };
        return { GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }

    HWND wallpaper_hwnd(int sw, int sh) {
        HWND progman = FindWindowA("Progman", nullptr);
        if (!progman) return nullptr;

        SendMessageTimeoutA(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);

        HWND worker = FindWindowExA(progman, nullptr, "WorkerW", nullptr);
        while (worker) {
            RECT r{};
            GetWindowRect(worker, &r);
            int w = r.right - r.left;
            int h = r.bottom - r.top;
            if (std::abs(w - sw) <= 10 && std::abs(h - sh) <= 10)
                return worker;
            worker = FindWindowExA(progman, worker, "WorkerW", nullptr);
        }
        return progman;
    }

    LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    HWND create_wallpaper_child(HINSTANCE inst, HWND parent, int w, int h) {
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = inst;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = "LiveWP";
        RegisterClassExA(&wc);

        HWND hwnd = CreateWindowExA(0, "LiveWP", "wp",
            WS_CHILD | WS_VISIBLE,
            0, 0, w, h,
            parent, nullptr, inst, nullptr);
        if (!hwnd) throw std::runtime_error("CreateWindowExA failed.");
        return hwnd;
    }

}


namespace sdl {

    struct TextureDeleter {
        void operator()(SDL_Texture* p) const { SDL_DestroyTexture(p); }
    };
    using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;

    struct RendererDeleter {
        void operator()(SDL_Renderer* p) const { SDL_DestroyRenderer(p); }
    };
    using RendererPtr = std::unique_ptr<SDL_Renderer, RendererDeleter>;

    struct WindowDeleter {
        void operator()(SDL_Window* p) const { SDL_DestroyWindow(p); }
    };
    using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;

}


class Renderer {
public:
    Renderer(HWND hwnd, int w, int h) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, hwnd);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
        window_.reset(SDL_CreateWindowWithProperties(props));
        SDL_DestroyProperties(props);

        if (!window_) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());

        renderer_.reset(SDL_CreateRenderer(window_.get(), nullptr));
        if (!renderer_) throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
    }

    void run(FrameQueue& queue, std::atomic<bool>& cancelled,
        std::atomic<bool>& decoder_done) {
        sdl::TexturePtr texture;

        double pts_origin = -1.0;
        double wall_origin = 0.0;

        double pts_offset = 0.0;
        double last_pts = 0.0;
        bool   first_frame = true;

        while (!cancelled) {
            pump_events(cancelled);

            YUVFrame frame;
            if (!queue.pop(frame, cancelled, decoder_done)) break;

            if (!first_frame && frame.pts_seconds < last_pts - 0.5)
                pts_offset += last_pts;
            last_pts = frame.pts_seconds;

            double adjusted_pts = frame.pts_seconds + pts_offset;

            if (first_frame) {
                pts_origin = adjusted_pts;
                wall_origin = static_cast<double>(SDL_GetTicks()) / 1000.0;
                texture = create_texture(frame.width, frame.height);
                first_frame = false;
            }

            double elapsed = static_cast<double>(SDL_GetTicks()) / 1000.0 - wall_origin;
            double frame_time = adjusted_pts - pts_origin;
            double wait_sec = frame_time - elapsed;
            if (wait_sec > 0.001)
                SDL_Delay(static_cast<Uint32>(wait_sec * 1000.0));

            upload_and_present(texture.get(), frame);
        }
    }

private:
    sdl::TexturePtr create_texture(int w, int h) {
        SDL_Texture* raw = SDL_CreateTexture(renderer_.get(),
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            w, h);
        if (!raw) throw std::runtime_error(std::string("SDL_CreateTexture: ") + SDL_GetError());
        return sdl::TexturePtr(raw);
    }

    void upload_and_present(SDL_Texture* tex, const YUVFrame& frame) {
        SDL_UpdateYUVTexture(tex, nullptr,
            frame.y.data(), frame.linesize_y,
            frame.u.data(), frame.linesize_u,
            frame.v.data(), frame.linesize_v);
        SDL_RenderClear(renderer_.get());
        SDL_RenderTexture(renderer_.get(), tex, nullptr, nullptr);
        SDL_RenderPresent(renderer_.get());
    }

    static void pump_events(std::atomic<bool>& cancelled) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { cancelled = true; return; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_EVENT_QUIT) { cancelled = true; return; }
    }

    sdl::WindowPtr   window_;
    sdl::RendererPtr renderer_;
};

int main(int argc, char* argv[]) {
    const char* video_path = (argc > 1) ? argv[1] : "video.mp4";

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    try {
        auto [sw, sh] = wallpaper::physical_screen_size();

        HWND parent = wallpaper::wallpaper_hwnd(sw, sh);
        if (!parent) throw std::runtime_error("Cannot obtain wallpaper HWND.");

        HINSTANCE inst = GetModuleHandle(nullptr);
        HWND child_hwnd = wallpaper::create_wallpaper_child(inst, parent, sw, sh);

        if (!SDL_Init(SDL_INIT_VIDEO))
            throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());

        constexpr std::size_t QUEUE_CAPACITY = 8;
        FrameQueue        queue(QUEUE_CAPACITY);
        std::atomic<bool> cancelled{ false };

        Decoder decoder(video_path, queue, cancelled);
        decoder.start();

        Renderer renderer(child_hwnd, sw, sh);
        renderer.run(queue, cancelled, decoder.done_flag());

        cancelled = true;
        queue.cancel();
        decoder.join();

        SDL_Quit();
        DestroyWindow(child_hwnd);

    }
    catch (const std::exception& ex) {
        std::cerr << "[livewp] Error: " << ex.what() << '\n';
        SDL_Quit();
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
