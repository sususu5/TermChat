#include "webserver.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include "../log/log.h"
#include "../pool/scylla_session.h"

namespace {
std::string ErrnoMessage(int err) {
    if (err == 0) {
        return "none";
    }
    return std::string(std::strerror(err));
}

std::string EpollEventSummary(uint32_t events) {
    std::string summary;
    auto append = [&summary](const char* name) {
        if (!summary.empty()) {
            summary += "|";
        }
        summary += name;
    };
    if (events & EPOLLIN) append("EPOLLIN");
    if (events & EPOLLOUT) append("EPOLLOUT");
    if (events & EPOLLRDHUP) append("EPOLLRDHUP");
    if (events & EPOLLHUP) append("EPOLLHUP");
    if (events & EPOLLERR) append("EPOLLERR");
    if (events & EPOLLONESHOT) append("EPOLLONESHOT");
    if (events & EPOLLET) append("EPOLLET");
    return summary.empty() ? "none" : summary;
}
}  // namespace

Webserver::Webserver(int port, int trig_mode, int timeout_ms, int sql_port, const char* sql_user, const char* sql_pwd,
                     const char* db_name, int conn_pool_num, int thread_num, int epoll_event_num, bool open_log,
                     int log_level, int log_que_size)
    : port_(port), timeout_ms_(timeout_ms), is_close_(false), timer_(new HeapTimer()),
      thread_pool_(new ThreadPool(thread_num)), epoller_(new Epoller(epoll_event_num)),
      push_service_(new PushService()), auth_service_(new AuthService()),
      friend_service_(new FriendService(push_service_.get())), msg_service_(new MsgService(push_service_.get())) {
    const char* sql_env_host = getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "localhost";

    if (open_log) {
        Log::instance()->init(log_level, "./log", ".log", log_que_size);
        if (is_close_) {
            spdlog::error("========== Server init error!==========");
        } else {
            spdlog::info("========== Server init ==========");
            spdlog::info("Listen Mode: {}, OpenConn Mode: {}", (listen_event_ & EPOLLET ? "ET" : "LT"),
                         (conn_event_ & EPOLLET ? "ET" : "LT"));
            spdlog::info("LogSys level: {}", log_level);
            spdlog::info("src_dir: {}", TcpConnection::src_dir);
            spdlog::info("MySQL Host: {}", sql_env_host);
            spdlog::info("SqlConnPool num: {}, ThreadPool num: {}, EpollEvent num: {}", conn_pool_num, thread_num,
                         epoll_event_num);
        }
    } else {
        spdlog::set_level(spdlog::level::off);
    }

    // Initialize services
    src_dir_ = getcwd(nullptr, 256);
    assert(src_dir_);
    strcat(src_dir_, "/resources/");
    TcpConnection::user_count = 0;
    TcpConnection::src_dir = src_dir_;
    TcpConnection::auth_service = auth_service_.get();
    TcpConnection::friend_service = friend_service_.get();
    TcpConnection::push_service = push_service_.get();
    TcpConnection::msg_service = msg_service_.get();
    TcpConnection::epoller_ = epoller_.get();
    TcpConnection::thread_pool = thread_pool_.get();

    // Initialize timer callback
    timer_->SetCallBack([this](int fd) {
        if (connections_.count(fd)) {
            CloseConn_(connections_[fd].get(), "idle timeout");
        }
    });

    // Initialize MySQL connection pool and Scylla session
    SqlConnPool::Instance()->Init(sql_env_host, sql_port, sql_user, sql_pwd, db_name, conn_pool_num);
    const char* scylla_host = getenv("SCYLLA_HOST") ? getenv("SCYLLA_HOST") : "scylla";
    const char* scylla_user = getenv("SCYLLA_USERNAME");
    const char* scylla_pwd = getenv("SCYLLA_PASSWORD");
    uint16_t scylla_port = 9042;
    if (const char* scylla_port_str = getenv("SCYLLA_PORT"); scylla_port_str && *scylla_port_str) {
        scylla_port = static_cast<uint16_t>(std::strtoul(scylla_port_str, nullptr, 10));
    }
    if (!ScyllaSession::Instance()->Init(scylla_host, scylla_port, scylla_user, scylla_pwd)) {
        spdlog::warn("Scylla session init failed. Message persistence may be unavailable.");
    } else {
        spdlog::info("Scylla session initialized successfully.");
    }
    InitEventMode_(trig_mode);
    if (!InitSocket_()) {
        is_close_ = true;
    }
}

Webserver::~Webserver() {
    spdlog::info("========== Server shutting down ==========");
    close(listen_fd_);
    is_close_ = true;
    free(src_dir_);
    SqlConnPool::Instance()->ClosePool();
    ScyllaSession::Instance()->Close();
    spdlog::info("========== Server stopped ==========");
    Log::instance()->flush();
}

void Webserver::InitEventMode_(int trig_mode) {
    listen_event_ = EPOLLRDHUP;
    conn_event_ = EPOLLONESHOT | EPOLLRDHUP;
    switch (trig_mode) {
        case 0:
            break;
        case 1:
            conn_event_ |= EPOLLET;
            break;
        case 2:
            listen_event_ |= EPOLLET;
            break;
        case 3:
            listen_event_ |= EPOLLET;
            conn_event_ |= EPOLLET;
            break;
        default:
            listen_event_ |= EPOLLET;
            conn_event_ |= EPOLLET;
            break;
    }
    TcpConnection::is_et = (conn_event_ & EPOLLET);
}

void Webserver::Start() {
    int time_ms = -1;
    if (!is_close_) {
        spdlog::info("========== Server start ==========");
    }
    while (!is_close_) {
        if (timeout_ms_ > 0) {
            time_ms = timer_->GetNextTick();
        }
        int event_cnt = epoller_->Wait(time_ms);
        for (int i = 0; i < event_cnt; i++) {
            int fd = epoller_->getEventFd(i);
            uint32_t events = epoller_->getEvents(i);
            // If the file descriptor is the listen socket, deal with the new connection
            if (fd == listen_fd_) {
                DealListen_();
            }
            // If the file descriptor is an error, close the connection
            else if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                CloseConn_(connections_.at(fd).get(), "epoll event: " + EpollEventSummary(events));
            }
            // If the file descriptor is readable, deal with the read event
            else if (events & EPOLLIN) {
                assert(connections_.count(fd) > 0);
                DealRead_(connections_[fd].get());
            }
            // If the file descriptor is writable, deal with the write event
            else if (events & EPOLLOUT) {
                assert(connections_.count(fd) > 0);
                DealWrite_(connections_[fd].get());
            } else {
                spdlog::error("Unexpected event");
            }
        }
    }
}

void Webserver::SendError_(int fd, const char* info) {
    assert(fd > 0);
    int ret = send(fd, info, strlen(info), 0);
    if (ret < 0) {
        spdlog::warn("send error to client[{}] error!", fd);
    }
    close(fd);
}

void Webserver::CloseConn_(TcpConnection* client, const std::string& reason) {
    assert(client);
    spdlog::info("Client[{}] closing, reason={}, to_write_bytes={}, keep_alive={}", client->get_fd(), reason,
                 client->to_write_bytes(), client->is_keep_alive());
    epoller_->delFd(client->get_fd());
    if (timeout_ms_ > 0) {
        timer_->Remove(client->get_fd());
    }
    client->close_conn();
}

void Webserver::AddClient_(int fd, sockaddr_in addr) {
    assert(fd > 0);
    auto conn = std::make_unique<TcpConnection>();
    conn->init(fd, addr);
    TcpConnection* conn_ptr = conn.get();
    connections_[fd] = std::move(conn);
    if (timeout_ms_ > 0) {
        timer_->Add(fd, timeout_ms_);
    }
    if (epoller_->addFd(fd, EPOLLIN | conn_event_)) {
        conn_ptr->UpdateEvents(EPOLLIN | conn_event_);
    }
    SetFdNonblock(fd);
    spdlog::info("Client[{}] in!", conn_ptr->get_fd());
}

void Webserver::DealListen_() {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    // In ET mode, the loop is used to accept all incoming connections
    do {
        int fd = accept(listen_fd_, (struct sockaddr*)&addr, &len);
        if (fd <= 0) {
            return;
        } else if (TcpConnection::user_count >= MAX_FD) {
            SendError_(fd, "Server busy!");
            spdlog::warn("Clients is full!");
            return;
        }
        AddClient_(fd, addr);
    } while (listen_event_ & EPOLLET);
}

void Webserver::DealRead_(TcpConnection* client) {
    assert(client);
    ExtendTime_(client);
    thread_pool_->AddTask(std::bind(&Webserver::OnRead_, this, client));
}

void Webserver::DealWrite_(TcpConnection* client) {
    assert(client);
    ExtendTime_(client);
    thread_pool_->AddTask(std::bind(&Webserver::OnWrite_, this, client));
}

void Webserver::ExtendTime_(TcpConnection* client) {
    assert(client);
    if (timeout_ms_ > 0) {
        timer_->Adjust(client->get_fd(), timeout_ms_);
    }
}

void Webserver::OnRead_(TcpConnection* client) {
    assert(client);
    int ret = -1;
    int readErrno = 0;
    ret = client->read(&readErrno);
    if (ret <= 0 && readErrno != EAGAIN) {
        CloseConn_(client,
                   std::format("read failed: ret={}, errno={}, error={}", ret, readErrno, ErrnoMessage(readErrno)));
        return;
    }
    OnProcess_(client);
}

// Resolve the request data
void Webserver::OnProcess_(TcpConnection* client) {
    if (client->process()) {
        // If the parsing succeeds, modify the event to EPOLLOUT(write)
        uint32_t events = conn_event_ | EPOLLOUT;
        epoller_->modFd(client->get_fd(), events);
        client->UpdateEvents(events);
    } else {
        // If the parsing fails, modify the event to EPOLLIN(read)
        uint32_t events = conn_event_ | EPOLLIN;
        epoller_->modFd(client->get_fd(), events);
        client->UpdateEvents(events);
    }
}

void Webserver::OnWrite_(TcpConnection* client) {
    assert(client);
    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);
    const auto remaining = client->to_write_bytes();
    if (remaining == 0) {
        uint32_t events = conn_event_ | EPOLLIN;
        epoller_->modFd(client->get_fd(), events);
        client->UpdateEvents(events);
        return;
    }

    if (ret >= 0 || writeErrno == EAGAIN || writeErrno == 0) {
        uint32_t events = conn_event_ | EPOLLOUT;
        epoller_->modFd(client->get_fd(), events);
        client->UpdateEvents(events);
        return;
    }

    CloseConn_(client, std::format("write close path: ret={}, errno={}, error={}, remaining={}, keep_alive={}", ret,
                                   writeErrno, ErrnoMessage(writeErrno), remaining, client->is_keep_alive()));
}

bool Webserver::InitSocket_() {
    int ret;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    // Create a socket
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        spdlog::error("Create socket error! port:{}", port_);
        return false;
    }

    // Set the socket to reuse the address
    int optval = 1;
    ret = setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    if (ret == -1) {
        spdlog::error("Set socket error!");
        close(listen_fd_);
        return false;
    }

    // Bind the socket to the address
    ret = bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        spdlog::error("Bind Port:{} error!", port_);
        close(listen_fd_);
        return false;
    }

    // Start listening. Use the kernel's configured maximum accept backlog so short connection bursts do not
    // overflow a tiny application-level queue during benchmark ramp-up.
    constexpr int kListenBacklog = SOMAXCONN;
    ret = listen(listen_fd_, kListenBacklog);
    if (ret < 0) {
        spdlog::error("Listen port:{} error!", port_);
        close(listen_fd_);
        return false;
    }

    // Add the listen socket to the epoll
    ret = epoller_->addFd(listen_fd_, EPOLLIN | listen_event_);
    if (ret == 0) {
        spdlog::error("Add listen error!");
        close(listen_fd_);
        return false;
    }
    SetFdNonblock(listen_fd_);
    spdlog::info("Server port:{}, listen backlog:{}", port_, kListenBacklog);
    return true;
}

int Webserver::SetFdNonblock(int fd) {
    assert(fd > 0);
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return flags;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
