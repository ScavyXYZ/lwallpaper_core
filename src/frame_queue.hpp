#pragma once

#include <d3d11.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>

extern "C" {
#include <libavutil/frame.h>
}

namespace livewallpaper::detail {

struct AvFrameDeleter {
    void operator()(AVFrame* p) const { av_frame_free(&p); }
};
using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

// A decoded frame handed from the decoder thread to the render thread.
//
// Depending on the render path in use this either owns
//  - a raw D3D11 surface (hw surface, zero-copy path), or
//  - ordinary CPU-side YUV420P / NV12 plane data (fallback path).
//
// Move-only: the frame owns a reference-counted ffmpeg buffer, so exactly one
// object may own it at a time. A raw pointer with a freeing destructor would
// double-free, because a defaulted move leaves the source still pointing at the
// same allocation.
struct VideoFrame {
    AvFramePtr frame;
    double ptsSeconds = 0.0;
    int width = 0;
    int height = 0;
    bool isNv12 = false;
    bool isRawHwFrame = false;

    VideoFrame() = default;
    explicit VideoFrame(AVFrame* f) : frame(f) {}
    VideoFrame(VideoFrame&&) noexcept = default;
    VideoFrame& operator=(VideoFrame&&) noexcept = default;
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
};

inline constexpr std::size_t kFrameQueueCapacity = 3;

class FrameQueue {
public:
    explicit FrameQueue(std::size_t capacity) : capacity_(capacity) {}

    // Returns false if cancelled while waiting for room.
    bool push(VideoFrame frame, const std::atomic<bool>& cancelled) {
        std::unique_lock<std::mutex> lock(mutex_);
        cvPush_.wait(lock, [&] { return queue_.size() < capacity_ || cancelled.load(); });
        if (cancelled.load()) return false;
        queue_.push(std::move(frame));
        cvPop_.notify_one();
        return true;
    }

    // Returns false when cancelled or when the producer finished and the queue
    // has been drained.
    bool pop(VideoFrame& out, const std::atomic<bool>& cancelled,
             const std::atomic<bool>& producerDone) {
        std::unique_lock<std::mutex> lock(mutex_);
        cvPop_.wait(lock, [&] {
            return !queue_.empty() || producerDone.load() || cancelled.load();
        });
        if (cancelled.load() || (queue_.empty() && producerDone.load())) return false;
        out = std::move(queue_.front());
        queue_.pop();
        cvPush_.notify_one();
        return true;
    }

    void cancel() {
        cvPush_.notify_all();
        cvPop_.notify_all();
    }

private:
    std::size_t          capacity_;
    std::queue<VideoFrame> queue_;
    std::mutex           mutex_;
    std::condition_variable cvPush_, cvPop_;
};

} // namespace livewallpaper::detail
