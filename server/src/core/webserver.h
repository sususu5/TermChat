#pragma once

#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <unordered_map>
#include "../pool/threadpool.h"
#include "../service/auth_service.h"
#include "../service/friend_service.h"
#include "../service/msg_service.h"
#include "../service/push_service.h"
#include "../timer/heaptimer.h"
#include "epoller.h"
#include "tcp_connection.h"

class Webserver {
public:
    Webserver(int port, int trig_mode, int timeout_ms, int sql_port, const char* sql_user, const char* sql_pwd,
              const char* db_name, int conn_pool_num, int thread_num, int epoll_event_num, bool open_log, int log_level,
              int log_que_size);
    ~Webserver();
    void Start();
    void Stop() { is_close_ = true; }

private:
    bool InitSocket_();
    void InitEventMode_(int trig_mode);
    void AddClient_(int fd, sockaddr_in addr);

    void DealListen_();
    void DealWrite_(TcpConnection* client);
    void DealRead_(TcpConnection* client);

    void SendError_(int fd, const char* info);
    void ExtendTime_(TcpConnection* client);
    void CloseConn_(TcpConnection* client, const std::string& reason);

    void OnRead_(TcpConnection* client);
    void OnWrite_(TcpConnection* client);
    void OnProcess_(TcpConnection* client);

    static const int MAX_FD = 65536;
    static int SetFdNonblock(int fd);

    int port_;
    bool open_linger_;
    int timeout_ms_;
    bool is_close_;
    int listen_fd_;
    char* src_dir_;

    uint32_t listen_event_;
    uint32_t conn_event_;

    std::unique_ptr<HeapTimer> timer_;
    std::unique_ptr<ThreadPool> thread_pool_;
    std::unique_ptr<Epoller> epoller_;
    std::unique_ptr<PushService> push_service_;
    std::unique_ptr<AuthService> auth_service_;
    std::unique_ptr<FriendService> friend_service_;
    std::unique_ptr<MsgService> msg_service_;
    // Store connections by fd, using unique_ptr since TcpConnection is not copyable
    std::unordered_map<int, std::unique_ptr<TcpConnection>> connections_;
};
