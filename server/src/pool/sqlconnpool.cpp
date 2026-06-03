#include "sqlconnpool.h"
#include <spdlog/spdlog.h>

namespace {
bool EnsureSchema(sqlpp::mysql::connection& conn) {
    try {
        conn.execute(
            "CREATE TABLE IF NOT EXISTS im_user ("
            "id INT AUTO_INCREMENT PRIMARY KEY,"
            "user_id BIGINT UNSIGNED NOT NULL UNIQUE,"
            "username VARCHAR(255) NOT NULL UNIQUE,"
            "password VARCHAR(255) NOT NULL"
            ");");
        conn.execute(
            "CREATE TABLE IF NOT EXISTS im_friend ("
            "id INT AUTO_INCREMENT PRIMARY KEY,"
            "user_id BIGINT UNSIGNED NOT NULL,"
            "friend_id BIGINT UNSIGNED NOT NULL,"
            "status INT NOT NULL,"
            "verify_msg VARCHAR(255) NOT NULL DEFAULT '',"
            "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
            "UNIQUE KEY uk_user_friend (user_id, friend_id),"
            "INDEX idx_friend_id (friend_id),"
            "INDEX idx_user_status (user_id, status)"
            ");");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("MYSQL schema initialization failed: {}", e.what());
        return false;
    }
}
}  // namespace

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

    int created = 0;
    for (int i = 0; i < connSize; i++) {
        try {
            auto conn = new sqlpp::mysql::connection(config);
            conn_queue_.emplace(conn);
            created++;
        } catch (const std::exception& e) {
            spdlog::error("MYSQL init error: {}", e.what());
        }
    }
    MAX_CONN_ = created;
    sem_init(&semId_, 0, MAX_CONN_);

    if (!conn_queue_.empty()) {
        EnsureSchema(*conn_queue_.front());
    }

    if (MAX_CONN_ == 0) {
        spdlog::error("MYSQL connection pool initialized with no available connections.");
    } else {
        spdlog::info("MYSQL connection pool initialized successfully: {}/{} connections.", MAX_CONN_, connSize);
    }
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
