#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include "protocol.pb.h"

class TcpConnection;

class PushService {
public:
    PushService() = default;
    ~PushService() = default;

    // Session Management
    void add_client(uint64_t user_id, TcpConnection* conn);
    void remove_client(uint64_t user_id);

    // Push Logic
    void push_friend_req(uint64_t req_id, uint64_t sender_id, const std::string& sender_name, uint64_t receiver_id,
                         const std::string& verify_msg);
    void push_friend_status(uint64_t sender_id, uint64_t receiver_id, const std::string& receiver_name,
                            const im::FriendAction& action);
    bool push_p2p_message(const im::P2PMessage& msg);

    // Generic push
    void push_to_user(uint64_t user_id, std::string data);

private:
    std::mutex mtx_;
    std::unordered_map<uint64_t, TcpConnection*> online_connections_;

    bool send_envelope(uint64_t receiver_id, const im::Envelope& envelope);
};
