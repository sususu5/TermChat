#pragma once

#include <spdlog/spdlog.h>
#include "../pool/sqlconnpool.h"

template <typename Table, typename KeyType>
class BaseDao {
public:
    explicit BaseDao() = default;
    virtual ~BaseDao() = default;

protected:
    Table table_{};

    // RAII wrapper for a MySQL connection
    class ConnGuard {
    public:
        ConnGuard() { conn_ = SqlConnPool::Instance()->GetConn(); }
        ~ConnGuard() {
            if (conn_) SqlConnPool::Instance()->FreeConn(conn_);
        }

        ConnGuard(const ConnGuard&) = delete;
        ConnGuard& operator=(const ConnGuard&) = delete;

        ConnGuard(ConnGuard&& other) noexcept : conn_(other.conn_) { other.conn_ = nullptr; }
        ConnGuard& operator=(ConnGuard&& other) noexcept {
            if (this != &other) {
                if (conn_) SqlConnPool::Instance()->FreeConn(conn_);
                conn_ = other.conn_;
                other.conn_ = nullptr;
            }
            return *this;
        }

        sqlpp::mysql::connection* operator->() const { return conn_; }
        sqlpp::mysql::connection& operator*() const { return *conn_; }
        operator bool() const { return conn_ != nullptr; }

    private:
        sqlpp::mysql::connection* conn_;
    };

    ConnGuard get_conn_guard() { return ConnGuard(); }

    template <typename Func, typename T>
    T execute(Func&& operation, const char* error_msg, T default_value) {
        auto conn = get_conn_guard();
        if (!conn) {
            spdlog::error("{}: Get Database Connection failed!", error_msg);
            return default_value;
        }

        try {
            return operation(*conn);
        } catch (const std::exception& e) {
            spdlog::error("{}: {}", error_msg, e.what());
            return default_value;
        }
    }

    template <typename Func>
    bool execute_bool(Func&& operation, const char* error_msg) {
        return execute(std::forward<Func>(operation), error_msg, false);
    }

    template <typename... Args>
    bool insert(const char* error_msg, Args&&... assignments) {
        return execute_bool(
            [&](auto& conn) {
                conn(sqlpp::insert_into(table_).set(std::forward<Args>(assignments)...));
                return true;
            },
            error_msg);
    }

    template <typename Condition>
    bool exists(const char* error_msg, Condition&& condition) {
        return execute_bool(
            [&](auto& conn) {
                auto result =
                    conn(sqlpp::select(sqlpp::count(1)).from(table_).where(std::forward<Condition>(condition)));
                return result.front().count > 0;
            },
            error_msg);
    }

    bool delete_by_id(const char* error_msg, const KeyType& id) {
        return execute_bool(
            [&](auto& conn) {
                conn(sqlpp::remove_from(table_).where(table_.id == id));
                return true;
            },
            error_msg);
    }

    template <typename Condition>
    int64_t count(const char* error_msg, Condition&& condition) {
        return execute(
            [&](auto& conn) -> int64_t {
                auto result =
                    conn(sqlpp::select(sqlpp::count(1)).from(table_).where(std::forward<Condition>(condition)));
                return result.front().count;
            },
            error_msg, int64_t{0});
    }

    int64_t count_all(const char* error_msg) {
        return execute(
            [&](auto& conn) -> int64_t {
                auto result = conn(sqlpp::select(sqlpp::count(1)).from(table_).unconditionally());
                return result.front().count;
            },
            error_msg, int64_t{0});
    }
};
