#pragma once

#include <arpa/inet.h>
#include <sys/uio.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "../buffer/buffer.h"
#include "../pool/threadpool.h"
#include "mpsc_queue.h"

// Forward declarations
class HttpHandler;
class AuthService;
class FriendService;
class PushService;
class Epoller;
class MsgService;

// ProtocolHandler is a pure virtual class that defines the interface for processing protocol data
class ProtocolHandler {
public:
    virtual ~ProtocolHandler() = default;

    virtual bool Process(Buffer& read_buff, Buffer& write_buff) = 0;
    virtual bool IsKeepAlive() const { return false; }
};

enum class ConnType {
    HTTP,
    PROTOBUF,
};

class TcpConnection {
public:
    TcpConnection() = default;
    ~TcpConnection();

    void init(int socket_fd, const sockaddr_in& addr);

    ssize_t read(int* error_code);
    ssize_t write(int* error_code);

    void close_conn();

    bool process();

    bool is_keep_alive() const;
    ConnType get_type() const;

    int get_fd() const { return fd_; }
    int get_port() const { return ntohs(addr_.sin_port); }
    // Change machine-readable address to human-readable address
    const char* get_ip() const { return inet_ntoa(addr_.sin_addr); }
    sockaddr_in get_addr() const { return addr_; }
    void UpdateEvents(uint32_t ev) { events_ = ev; }

    Buffer& get_read_buffer() { return read_buff_; }
    Buffer& get_write_buffer() { return write_buff_; }
    size_t to_write_bytes();

    void set_user_id(uint64_t user_id);
    uint64_t get_user_id() const { return user_id_; }
    bool is_logged_in() const { return user_id_ != 0; }

    // Message queue for push service
    void enqueue_message(std::string data);
    void enqueue_messages(std::vector<std::string> messages);
    bool flush_pending_to_buffer();
    bool has_pending_messages() const { return !outgoing_queue_.empty(); }

    // Represent whether the server is using ET mode
    static bool is_et;
    // Store the source directory of the server
    static const char* src_dir;
    static std::atomic<int> user_count;

    // Service dependencies (injected by Webserver)
    static AuthService* auth_service;
    static FriendService* friend_service;
    static PushService* push_service;
    static MsgService* msg_service;
    static Epoller* epoller_;
    static ThreadPool* thread_pool;

protected:
    int fd_{-1};
    // Store the network address info of the client
    struct sockaddr_in addr_ {};
    bool is_close_{true};

    std::mutex conn_mutex_;
    Buffer read_buff_;
    Buffer write_buff_;
    std::unique_ptr<ProtocolHandler> handler_;

    // iovec for zero-copy file sending (used by HTTP)
    struct iovec iov_[2]{};
    int iov_cnt_{0};

    bool protocol_determined_{false};
    ConnType conn_type_{ConnType::HTTP};

    MPSCQueue<std::string> outgoing_queue_;

private:
    void setup_iov_for_http();
    void notify_writable();

    uint64_t user_id_{0};
    std::atomic<uint32_t> events_{0};
};
