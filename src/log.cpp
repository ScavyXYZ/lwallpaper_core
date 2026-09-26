#include "log.hpp"

#include <cstdio>
#include <iostream>

namespace livewallpaper::detail {
namespace {

std::mutex& sinkMutex() {
    static std::mutex m;
    return m;
}

LogCallback& sink() {
    static LogCallback cb;
    return cb;
}

LogLevel& level() {
    static LogLevel lvl = LogLevel::Info;
    return lvl;
}

const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

} // namespace

void setLogCallback(LogCallback cb) {
    std::lock_guard<std::mutex> lock(sinkMutex());
    sink() = std::move(cb);
}

void setLogLevel(LogLevel l) {
    std::lock_guard<std::mutex> lock(sinkMutex());
    level() = l;
}

LogLevel logLevel() {
    std::lock_guard<std::mutex> lock(sinkMutex());
    return level();
}

void emit(LogLevel lvl, const std::string& message) {
    LogCallback cb;
    {
        std::lock_guard<std::mutex> lock(sinkMutex());
        cb = sink();
    }
    if (cb) {
        cb(lvl, message);
        return;
    }
    if (lvl == LogLevel::Error || lvl == LogLevel::Warn) {
        std::cerr << "[" << levelName(lvl) << "] " << message << "\n";
    } else {
        std::clog << "[" << levelName(lvl) << "] " << message << "\n";
    }
}

} // namespace livewallpaper::detail
