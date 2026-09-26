#pragma once

#include <livewallpaper/livewallpaper.hpp>

#include <mutex>
#include <sstream>
#include <string>

namespace livewallpaper::detail {

void setLogCallback(LogCallback cb);
void setLogLevel(LogLevel level);
LogLevel logLevel();
void emit(LogLevel level, const std::string& message);

// Collects a message and emits it on destruction, but only when the level
// passes the filter, so an expensive message costs nothing when suppressed.
class LogStream {
public:
    explicit LogStream(LogLevel level) : level_(level) {}

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    ~LogStream() {
        if (enabled()) emit(level_, oss_.str());
    }

    bool enabled() const { return level_ >= logLevel(); }
    std::ostringstream& stream() { return oss_; }

private:
    LogLevel level_;
    std::ostringstream oss_;
};

} // namespace livewallpaper::detail

#define LW_LOG_DEBUG(msg)                                                        \
    if (auto lwp_log_ = ::livewallpaper::detail::LogStream(                      \
            ::livewallpaper::LogLevel::Debug); lwp_log_.enabled())               \
    lwp_log_.stream() << msg

#define LW_LOG_INFO(msg)                                                         \
    if (auto lwp_log_ = ::livewallpaper::detail::LogStream(                      \
            ::livewallpaper::LogLevel::Info); lwp_log_.enabled())                \
    lwp_log_.stream() << msg

#define LW_LOG_WARN(msg)                                                         \
    if (auto lwp_log_ = ::livewallpaper::detail::LogStream(                      \
            ::livewallpaper::LogLevel::Warn); lwp_log_.enabled())                \
    lwp_log_.stream() << msg

#define LW_LOG_ERROR(msg)                                                        \
    if (auto lwp_log_ = ::livewallpaper::detail::LogStream(                      \
            ::livewallpaper::LogLevel::Error); lwp_log_.enabled())               \
    lwp_log_.stream() << msg
