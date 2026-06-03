#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "../core/mpsc_queue.h"
#include "protocol.pb.h"

class TcpConnection;

class PushService {
public:
    PushService();
    ~PushService();

    // Session Management
    void add_client(uint64_t user_id, TcpConnection* conn);
    void remove_client(uint64_t user_id);

    // Push Logic
    void push_friend_req(uint64_t req_id, uint64_t sender_id, const std::string& sender_name, uint64_t receiver_id,
                         const std::string& verify_msg);
    void push_friend_status(uint64_t sender_id, uint64_t receiver_id, const std::string& receiver_name,
                            const im::FriendAction& action);
    bool push_p2p_message(const im::P2PMessage& msg);
    bool push_message_ack(uint64_t user_id, const im::MessageAck& ack);

    // Generic push
    void push_to_user(uint64_t user_id, std::string data);

private:
    struct ConnectionShard {
        std::mutex mutex;
        std::unordered_map<uint64_t, TcpConnection*> online_connections;
    };

    struct PushItem {
        uint64_t user_id{0};
        std::string data;
    };

    static constexpr size_t kShardCount = 128;
    static constexpr size_t kMaxBatchSize = 1024;

    std::array<ConnectionShard, kShardCount> shards_;
    MPSCQueue<PushItem> push_queue_;
    std::thread flush_worker_;
    std::atomic<bool> running_{false};

    ConnectionShard& ShardFor(uint64_t user_id);
    TcpConnection* FindConnection(uint64_t user_id);
    bool send_envelope(uint64_t receiver_id, const im::Envelope& envelope);
    bool enqueue_serialized(uint64_t user_id, std::string data);
    void FlushLoop();
    void FlushPending(size_t max_count);
};
