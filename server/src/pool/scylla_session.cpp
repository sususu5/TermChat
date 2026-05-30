#include "scylla_session.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <string>

namespace {
std::string CassFutureError(CassFuture* future) {
    const char* message = nullptr;
    size_t message_length = 0;
    cass_future_error_message(future, &message, &message_length);
    if (message == nullptr || message_length == 0) {
        return "unknown error";
    }
    return std::string(message, message_length);
}

bool has_value(const char* value) { return value != nullptr && std::strlen(value) > 0; }
}  // namespace

ScyllaSession* ScyllaSession::Instance() {
    static ScyllaSession instance;
    return &instance;
}

bool ScyllaSession::Init(const char* host, uint16_t port, const char* user, const char* pwd) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (initialized_) {
        return true;
    }

    cluster_ = cass_cluster_new();
    session_ = cass_session_new();
    if (!cluster_ || !session_) {
        spdlog::error("Scylla init failed: out of memory");
        Close();
        return false;
    }

    if (has_value(host)) {
        cass_cluster_set_contact_points(cluster_, host);
    } else {
        cass_cluster_set_contact_points(cluster_, "scylla");
    }
    if (port != 0) {
        cass_cluster_set_port(cluster_, port);
    }
    if (has_value(user) && has_value(pwd)) {
        cass_cluster_set_credentials(cluster_, user, pwd);
    }

    CassFuture* connect_future = cass_session_connect(session_, cluster_);
    cass_future_wait(connect_future);
    if (cass_future_error_code(connect_future) != CASS_OK) {
        auto err = CassFutureError(connect_future);
        cass_future_free(connect_future);
        spdlog::error("Scylla connect failed: {}", err);
        Close();
        return false;
    }

    cass_future_free(connect_future);
    initialized_ = true;
    spdlog::info("Scylla session initialized successfully.");
    return true;
}

void ScyllaSession::Close() {
    std::lock_guard<std::mutex> lock(mtx_);

    if (session_) {
        CassFuture* close_future = cass_session_close(session_);
        if (close_future) {
            cass_future_wait(close_future);
            cass_future_free(close_future);
        }
        cass_session_free(session_);
        session_ = nullptr;
    }

    if (cluster_) {
        cass_cluster_free(cluster_);
        cluster_ = nullptr;
    }

    initialized_ = false;
}

CassSession* ScyllaSession::Session() {
    std::lock_guard<std::mutex> lock(mtx_);
    return session_;
}

bool ScyllaSession::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return initialized_;
}
