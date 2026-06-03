#include "push_service.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include "../core/tcp_connection.h"
#include "protocol.pb.h"

PushService::PushService() {
    running_.store(true, std::memory_order_release);
    flush_worker_ = std::thread(&PushService::FlushLoop, this);
}

PushService::~PushService() {
    running_.store(false, std::memory_order_release);
    if (flush_worker_.joinable()) {
        flush_worker_.join();
    }
    while (!push_queue_.empty()) {
        FlushPending(kMaxBatchSize);
    }
}

void PushService::add_client(uint64_t user_id, TcpConnection* conn) {
    auto& shard = ShardFor(user_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.online_connections[user_id] = conn;
    spdlog::info("User[{}] registered for push service", user_id);
}

void PushService::remove_client(uint64_t user_id) {
    auto& shard = ShardFor(user_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (shard.online_connections.contains(user_id)) {
        shard.online_connections.erase(user_id);
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
    std::string serialized;
    if (!envelope.SerializeToString(&serialized)) {
        return false;
    }

    if (enqueue_serialized(target_id, std::move(serialized))) {
        spdlog::info("Push enqueued for User[{}], cmd={}", target_id, static_cast<int>(envelope.cmd()));
        return true;
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

void PushService::push_to_user(uint64_t user_id, std::string data) { enqueue_serialized(user_id, std::move(data)); }

PushService::ConnectionShard& PushService::ShardFor(uint64_t user_id) {
    return shards_[std::hash<uint64_t>{}(user_id) % shards_.size()];
}

TcpConnection* PushService::FindConnection(uint64_t user_id) {
    auto& shard = ShardFor(user_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.online_connections.find(user_id);
    if (it == shard.online_connections.end()) {
        return nullptr;
    }
    return it->second;
}

bool PushService::enqueue_serialized(uint64_t user_id, std::string data) {
    if (user_id == 0 || data.empty() || !FindConnection(user_id)) {
        return false;
    }

    push_queue_.enqueue(PushItem{user_id, std::move(data)});
    return true;
}

void PushService::FlushLoop() {
    while (running_.load(std::memory_order_acquire)) {
        FlushPending(kMaxBatchSize);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void PushService::FlushPending(size_t max_count) {
    std::vector<PushItem> items;
    items.reserve(max_count);
    push_queue_.dequeue_bulk(std::back_inserter(items), max_count);
    if (items.empty()) {
        return;
    }

    std::unordered_map<TcpConnection*, std::vector<std::string>> batches;
    batches.reserve(items.size());
    for (auto& item : items) {
        TcpConnection* conn = FindConnection(item.user_id);
        if (!conn) {
            continue;
        }
        batches[conn].push_back(std::move(item.data));
    }

    for (auto& [conn, messages] : batches) {
        conn->enqueue_messages(std::move(messages));
    }
}
