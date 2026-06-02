#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "message_service.pb.h"

class ClientDedupCache {
public:
    struct Entry {
        uint64_t server_msg_id = 0;
        uint64_t receiver_id = 0;
        im::MessageAckStatus status = im::ACK_STATUS_UNKNOWN;
    };

    enum class ReserveState {
        kReserved,
        kDuplicate,
        kConflict,
    };

    struct ReserveResult {
        ReserveState state = ReserveState::kReserved;
        Entry entry;
    };

    explicit ClientDedupCache(size_t shard_count = 256, std::chrono::milliseconds ttl = std::chrono::minutes(30),
                              size_t max_entries_per_shard = 8192);

    ReserveResult Reserve(uint64_t sender_id, uint64_t client_msg_id, uint64_t server_msg_id, uint64_t receiver_id,
                          im::MessageAckStatus status);
    void UpdateStatus(uint64_t sender_id, uint64_t client_msg_id, uint64_t server_msg_id, im::MessageAckStatus status);

private:
    struct Key {
        uint64_t sender_id = 0;
        uint64_t client_msg_id = 0;

        bool operator==(const Key& other) const {
            return sender_id == other.sender_id && client_msg_id == other.client_msg_id;
        }
    };

    struct KeyHash {
        size_t operator()(const Key& key) const {
            const uint64_t mixed = key.sender_id ^ (key.client_msg_id + 0x9e3779b97f4a7c15ULL + (key.sender_id << 6) +
                                                    (key.sender_id >> 2));
            return static_cast<size_t>(mixed ^ (mixed >> 32));
        }
    };

    struct CacheValue {
        Entry entry;
        std::chrono::steady_clock::time_point expires_at;
    };

    struct Shard {
        std::mutex mutex;
        std::unordered_map<Key, CacheValue, KeyHash> entries;
        std::chrono::steady_clock::time_point next_evict_at = std::chrono::steady_clock::time_point{};
    };

    Shard& ShardFor(const Key& key);
    void EvictExpiredLocked(Shard& shard, std::chrono::steady_clock::time_point now);

    std::vector<Shard> shards_;
    std::chrono::milliseconds ttl_;
    size_t max_entries_per_shard_;
};
