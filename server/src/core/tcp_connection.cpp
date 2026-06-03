#include "tcp_connection.h"
#include <errno.h>
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <unordered_set>
#include "../service/push_service.h"
#include "epoller.h"
#include "handler/http_handler.h"
#include "handler/protobuf_handler.h"

std::atomic<int> TcpConnection::user_count{0};
bool TcpConnection::is_et = false;
const char* TcpConnection::src_dir = "";
AuthService* TcpConnection::auth_service = nullptr;
FriendService* TcpConnection::friend_service = nullptr;
PushService* TcpConnection::push_service = nullptr;
Epoller* TcpConnection::epoller_ = nullptr;
MsgService* TcpConnection::msg_service = nullptr;
ThreadPool* TcpConnection::thread_pool = nullptr;

TcpConnection::~TcpConnection() { close_conn(); }

void TcpConnection::init(int socket_fd, const sockaddr_in& addr) {
    assert(socket_fd > 0);
    user_count++;
    addr_ = addr;
    fd_ = socket_fd;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        write_buff_.retrieve_all();
        read_buff_.retrieve_all();
    }
    is_close_ = false;
    protocol_determined_ = false;
    handler_.reset();
    iov_cnt_ = 0;
    user_id_ = 0;
    events_ = 0;
    spdlog::info("Client[{}]({}:{}) in, user_count:{}", fd_, get_ip(), get_port(), (int)user_count);
}

void TcpConnection::close_conn() {
    if (!is_close_) {
        is_close_ = true;
        user_count--;

        if (user_id_ != 0 && push_service) {
            push_service->remove_client(user_id_);
        }

        close(fd_);
        spdlog::info("Client[{}]({}:{}) quit, user_count:{}", fd_, get_ip(), get_port(), (int)user_count);
    }
}

void TcpConnection::set_user_id(uint64_t user_id) {
    user_id_ = user_id;
    if (push_service) {
        push_service->add_client(user_id, this);
    }
}

static bool is_http_request(const char* data, size_t len) {
    if (len < 4) {
        return false;
    }
    static const std::unordered_set<std::string> methods = {"GET ", "POST", "HEAD", "PUT ", "DELE"};
    return methods.find(std::string(data, 4)) != methods.end();
}

bool TcpConnection::process() {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (!protocol_determined_) {
        if (read_buff_.readable_bytes() == 0) {
            return false;
        }

        const char* data = read_buff_.peek();
        auto len = read_buff_.readable_bytes();
        if (is_http_request(data, len)) {
            handler_ = std::make_unique<HttpHandler>();
            conn_type_ = ConnType::HTTP;
            spdlog::info("Protocol determined: HTTP");
        } else {
            handler_ = std::make_unique<ProtobufHandler>(this, auth_service, friend_service, msg_service, thread_pool);
            conn_type_ = ConnType::PROTOBUF;
            spdlog::info("Protocol determined: Protobuf");
        }
        protocol_determined_ = true;
    }

    if (!handler_) {
        return false;
    }

    if (conn_type_ == ConnType::HTTP) {
        if (handler_->Process(read_buff_, write_buff_)) {
            setup_iov_for_http();
            return true;
        }
        return false;
    }

    bool produced_response = false;
    while (handler_->Process(read_buff_, write_buff_)) {
        produced_response = true;
    }
    return produced_response;
}

void TcpConnection::setup_iov_for_http() {
    auto* http_handler = dynamic_cast<HttpHandler*>(handler_.get());
    if (!http_handler) return;

    // iov_[0] for response header in write buffer
    iov_[0] = write_buff_.ToIovec();
    iov_cnt_ = 1;

    // iov_[1] for mmap file content (zero-copy)
    if (http_handler->FileLen() > 0 && http_handler->File()) {
        iov_[1].iov_base = http_handler->File();
        iov_[1].iov_len = http_handler->FileLen();
        iov_cnt_ = 2;
    }

    spdlog::debug("HTTP iov setup: header={}, file={}", iov_[0].iov_len, iov_cnt_ > 1 ? iov_[1].iov_len : 0);
}

size_t TcpConnection::to_write_bytes() {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (conn_type_ == ConnType::HTTP && iov_cnt_ > 0) {
        return iov_[0].iov_len + (iov_cnt_ > 1 ? iov_[1].iov_len : 0);
    }
    return write_buff_.readable_bytes();
}

bool TcpConnection::is_keep_alive() const {
    if (handler_) {
        return handler_->IsKeepAlive();
    }
    return false;
}

ConnType TcpConnection::get_type() const { return conn_type_; }

ssize_t TcpConnection::read(int* error_code) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    ssize_t len = -1;
    do {
        len = read_buff_.read_fd(fd_, error_code);
        if (len <= 0) {
            break;
        }
    } while (is_et);
    return len;
}

ssize_t TcpConnection::write(int* error_code) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    ssize_t len = -1;

    // Flush queued push messages into write buffer before sending
    flush_pending_to_buffer();

    // Use writev for HTTP to support zero-copy file sending
    if (conn_type_ == ConnType::HTTP && iov_cnt_ > 0) {
        while (true) {
            len = writev(fd_, iov_, iov_cnt_);
            if (len <= 0) {
                *error_code = errno;
                break;
            }

            // Adjust iov after partial write
            if (static_cast<size_t>(len) > iov_[0].iov_len) {
                // Header fully sent, adjust file pointer
                size_t file_sent = len - iov_[0].iov_len;
                iov_[1].iov_base = static_cast<char*>(iov_[1].iov_base) + file_sent;
                iov_[1].iov_len -= file_sent;
                if (iov_[0].iov_len > 0) {
                    write_buff_.retrieve_all();
                    iov_[0].iov_len = 0;
                }
            } else {
                // Header partially sent
                iov_[0].iov_base = static_cast<char*>(iov_[0].iov_base) + len;
                iov_[0].iov_len -= len;
                write_buff_.retrieve(len);
            }

            size_t remaining = iov_[0].iov_len + (iov_cnt_ > 1 ? iov_[1].iov_len : 0);
            if (remaining == 0) break;
            // In ET mode, we must write until EAGAIN or empty
            if (!is_et && remaining < 10240) break;
        }
    } else {
        while (write_buff_.readable_bytes() > 0) {
            len = write_buff_.write_fd(fd_, error_code);
            if (len <= 0) {
                break;
            }
            // In ET mode, we must write until EAGAIN or empty
            // In LT mode, we stop after one write or continue if data is large to reduce events
            if (!is_et && write_buff_.readable_bytes() < 10240) {
                break;
            }
        }
    }
    return len;
}

void TcpConnection::enqueue_message(std::string data) {
    std::vector<std::string> messages;
    messages.emplace_back(std::move(data));
    enqueue_messages(std::move(messages));
}

void TcpConnection::enqueue_messages(std::vector<std::string> messages) {
    if (messages.empty()) {
        return;
    }

    const bool should_notify = outgoing_queue_.empty();
    for (auto& data : messages) {
        if (data.empty()) {
            continue;
        }

        std::string framed;
        framed.reserve(sizeof(uint32_t) + data.size());
        uint32_t msg_len = htonl(static_cast<uint32_t>(data.size()));
        framed.append(reinterpret_cast<char*>(&msg_len), 4);
        framed.append(data);
        outgoing_queue_.enqueue(std::move(framed));
    }

    if (should_notify) {
        notify_writable();
    }
}

bool TcpConnection::flush_pending_to_buffer() {
    bool has_data = false;

    std::optional<std::string> msg;
    while ((msg = outgoing_queue_.dequeue()).has_value()) {
        write_buff_.append(msg.value());
        has_data = true;
    }
    return has_data;
}

void TcpConnection::notify_writable() {
    uint32_t events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLONESHOT;
    if (!epoller_) return;
    if (is_et) events |= EPOLLET;
    if (events_ == events) return;

    if (epoller_->modFd(fd_, events)) {
        events_ = events;
    }
}
