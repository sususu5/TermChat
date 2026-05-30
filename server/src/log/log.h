#pragma once

#include <spdlog/spdlog.h>
#include <memory>
#include <mutex>

class Log {
public:
    void init(int level, const char* path = "./log", const char* suffix = ".log", int maxQueCapacity = 1024);

    static Log* instance();
    static void flush_log_thread();

    void flush();

    int get_level();
    void set_level(int level);
    bool is_open() const { return is_open_; }

private:
    Log() = default;
    ~Log();

    static spdlog::level::level_enum to_spdlog_level_(int level);

    std::shared_ptr<spdlog::logger> logger_;
    int level_ = 1;
    bool is_open_ = false;
    std::mutex mtx_;
};
