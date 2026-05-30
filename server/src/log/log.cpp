#include "log.h"
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>

namespace {
std::string LogFileName(const char* path, const char* suffix) {
    auto now = std::chrono::system_clock::now();
    auto now_days = std::chrono::floor<std::chrono::days>(now);
    auto ymd = std::chrono::year_month_day{now_days};
    return std::format("{}/{:%Y_%m_%d}{}", path, ymd, suffix);
}

spdlog::level::level_enum ToSpdlogLevel(int level) {
    switch (level) {
        case 0:
            return spdlog::level::debug;
        case 1:
            return spdlog::level::info;
        case 2:
            return spdlog::level::warn;
        case 3:
            return spdlog::level::err;
        default:
            return spdlog::level::info;
    }
}
}  // namespace

Log::~Log() {
    flush();
    std::lock_guard<std::mutex> locker(mtx_);
    if (logger_) {
        spdlog::drop(logger_->name());
        logger_.reset();
    }
}

int Log::get_level() {
    std::lock_guard<std::mutex> locker(mtx_);
    return level_;
}

void Log::set_level(int level) {
    std::lock_guard<std::mutex> locker(mtx_);
    level_ = level;
    if (logger_) {
        logger_->set_level(ToSpdlogLevel(level));
    }
}

void Log::init(int level, const char* path, const char* suffix, int maxQueueSize) {
    std::lock_guard<std::mutex> locker(mtx_);

    std::filesystem::create_directories(path);

    level_ = level;
    const auto logger_name = "termchat";
    const auto file_name = LogFileName(path, suffix);
    spdlog::drop(logger_name);

    if (maxQueueSize > 0) {
        spdlog::init_thread_pool(static_cast<size_t>(maxQueueSize), 1);
        logger_ = spdlog::basic_logger_mt<spdlog::async_factory>(logger_name, file_name);
    } else {
        logger_ = spdlog::basic_logger_mt(logger_name, file_name);
    }

    logger_->set_level(ToSpdlogLevel(level));
    logger_->flush_on(spdlog::level::err);
    logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%f] [%l] %v");
    spdlog::set_default_logger(logger_);
    spdlog::set_level(ToSpdlogLevel(level));
    spdlog::flush_on(spdlog::level::err);
    is_open_ = true;
}

void Log::flush() {
    auto logger = logger_;
    if (logger) {
        logger->flush();
    }
}

void Log::flush_log_thread() { Log::instance()->flush(); }

Log* Log::instance() {
    static Log inst;
    return &inst;
}

spdlog::level::level_enum Log::to_spdlog_level_(int level) { return ToSpdlogLevel(level); }
