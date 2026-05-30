#include "sqlconnpool.h"
#include <spdlog/spdlog.h>

SqlConnPool* SqlConnPool::Instance() {
    static SqlConnPool pool;
    return &pool;
}

auto SqlConnPool::Init(const char* host, uint16_t port, const char* user, const char* pwd, const char* dbName,
                       int connSize = 10) -> void {
    assert(connSize > 0);
    auto config = std::make_shared<sqlpp::mysql::connection_config>();
    config->host = host;
    config->port = port;
    config->user = user;
    config->password = pwd;
    config->database = dbName;
    config->ssl = true;
    config->ssl_ca = "/etc/mysql/certs/ca.pem";

    for (int i = 0; i < connSize; i++) {
        try {
            auto conn = new sqlpp::mysql::connection(config);
            conn_queue_.emplace(conn);
        } catch (const std::exception& e) {
            spdlog::error("MYSQL init error: {}", e.what());
        }
    }
    MAX_CONN_ = connSize;
    sem_init(&semId_, 0, MAX_CONN_);

    spdlog::info("MYSQL connection pool initialized successfully!");
}

auto SqlConnPool::GetConn() -> sqlpp::mysql::connection* {
    sqlpp::mysql::connection* conn = nullptr;
    if (conn_queue_.empty()) {
        spdlog::warn("SqlConnPool busy!");
        return nullptr;
    }
    // Check if the semaphore is greater than 0 which means there are available connections
    // If the semaphore is 0, the thread will be blocked
    sem_wait(&semId_);
    std::lock_guard<std::mutex> locker(mtx_);
    conn = conn_queue_.front();
    conn_queue_.pop();
    return conn;
}

auto SqlConnPool::FreeConn(sqlpp::mysql::connection* conn) -> void {
    assert(conn);
    std::lock_guard<std::mutex> locker(mtx_);
    conn_queue_.push(conn);
    sem_post(&semId_);
}

auto SqlConnPool::ClosePool() -> void {
    std::lock_guard<std::mutex> locker(mtx_);
    while (!conn_queue_.empty()) {
        auto conn = conn_queue_.front();
        conn_queue_.pop();
        delete conn;
    }
}

auto SqlConnPool::GetFreeConnCount() -> int {
    std::lock_guard<std::mutex> locker(mtx_);
    return conn_queue_.size();
}
