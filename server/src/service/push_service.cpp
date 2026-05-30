#include "push_service.h"
#include <spdlog/spdlog.h>
#include <ctime>
#include "../core/tcp_connection.h"
#include "protocol.pb.h"

void PushService::add_client(uint64_t user_id, TcpConnection* conn) {
    std::lock_guard<std::mutex> lock(mtx_);
    online_connections_[user_id] = conn;
    spdlog::info("User[{}] registered for push service", user_id);
}

void PushService::remove_client(uint64_t user_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (online_connections_.contains(user_id)) {
        online_connections_.erase(user_id);
        spdlog::info("User[{}] unregistered from push service", user_id);
    }
}

void PushService::push_friend_req(uint64_t req_id, uint64_t sender_id, const std::string& sender_name,
                                  uint64_t receiver_id, const std::string& verify_msg) {
    im::Envelope envelope;
    envelope.set_seq(0);
    envelope.set_cmd(im::CMD_FRIEND_REQ_PUSH);
    envelope.set_timestamp(time(nullptr));

    auto* push = envelope.mutable_friend_req_push();
    push->set_req_id(req_id);
    push->set_sender_id(sender_id);
    push->set_sender_name(sender_name);
    push->set_verify_msg(verify_msg);
    push->set_timestamp(time(nullptr));

    send_envelope(receiver_id, envelope);
}

bool PushService::send_envelope(uint64_t target_id, const im::Envelope& envelope) {
    TcpConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (online_connections_.contains(target_id)) {
            conn = online_connections_.at(target_id);
        }
    }

    if (conn) {
        std::string serialized;
        if (envelope.SerializeToString(&serialized)) {
            conn->enqueue_message(std::move(serialized));
            spdlog::info("Push enqueued for User[{}], cmd={}", target_id, static_cast<int>(envelope.cmd()));
            return true;
        }
    }
    return false;
}

void PushService::push_friend_status(uint64_t sender_id, uint64_t receiver_id, const std::string& receiver_name,
                                     const im::FriendAction& action) {
    im::Envelope envelope;
    envelope.set_seq(0);
    envelope.set_cmd(im::CMD_FRIEND_STATUS_PUSH);
    envelope.set_timestamp(time(nullptr));

    auto* push = envelope.mutable_friend_status_push();
    push->set_receiver_id(receiver_id);
    push->set_receiver_name(receiver_name);
    push->set_action(action);

    send_envelope(sender_id, envelope);
}

bool PushService::push_p2p_message(const im::P2PMessage& msg) {
    im::Envelope envelope;
    envelope.set_seq(0);
    envelope.set_cmd(im::CMD_P2P_MSG_PUSH);
    envelope.set_timestamp(time(nullptr));

    *envelope.mutable_p2p_msg_push() = msg;

    return send_envelope(msg.receiver_id(), envelope);
}

bool PushService::push_message_ack(uint64_t user_id, const im::MessageAck& ack) {
    im::Envelope envelope;
    envelope.set_seq(0);
    envelope.set_cmd(im::CMD_MSG_ACK);
    envelope.set_timestamp(time(nullptr));
    envelope.mutable_msg_ack()->CopyFrom(ack);

    return send_envelope(user_id, envelope);
}

void PushService::push_to_user(uint64_t user_id, std::string data) {
    TcpConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (online_connections_.contains(user_id)) {
            conn = online_connections_.at(user_id);
        }
    }

    if (conn) {
        conn->enqueue_message(std::move(data));
    }
}
