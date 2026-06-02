#include "client_dedup_cache.h"
#include <algorithm>

ClientDedupCache::ClientDedupCache(size_t shard_count, std::chrono::milliseconds ttl, size_t max_entries_per_shard)
    : shards_(std::max<size_t>(1, shard_count)), ttl_(ttl), max_entries_per_shard_(max_entries_per_shard) {}

ClientDedupCache::ReserveResult ClientDedupCache::Reserve(uint64_t sender_id, uint64_t client_msg_id,
                                                          uint64_t server_msg_id, uint64_t receiver_id,
                                                          im::MessageAckStatus status) {
    ReserveResult result;
    if (sender_id == 0 || client_msg_id == 0 || server_msg_id == 0 || receiver_id == 0) {
        result.state = ReserveState::kConflict;
        return result;
    }

    const Key key{sender_id, client_msg_id};
    auto now = std::chrono::steady_clock::now();
    auto& shard = ShardFor(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    EvictExpiredLocked(shard, now);

    if (shard.entries.contains(key)) {
        if (shard.entries.at(key).entry.receiver_id != receiver_id) {
            result.state = ReserveState::kConflict;
        } else {
            result.state = ReserveState::kDuplicate;
        }
        result.entry = shard.entries.at(key).entry;
        return result;
    }

    result.entry = Entry{server_msg_id, receiver_id, status};
    shard.entries.emplace(key, CacheValue{result.entry, now + ttl_});
    result.state = ReserveState::kReserved;
    return result;
}

void ClientDedupCache::UpdateStatus(uint64_t sender_id, uint64_t client_msg_id, uint64_t server_msg_id,
                                    im::MessageAckStatus status) {
    if (sender_id == 0 || client_msg_id == 0 || server_msg_id == 0) {
        return;
    }

    const Key key{sender_id, client_msg_id};
    auto now = std::chrono::steady_clock::now();
    auto& shard = ShardFor(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    EvictExpiredLocked(shard, now);

    auto it = shard.entries.find(key);
    if (it == shard.entries.end() || it->second.entry.server_msg_id != server_msg_id) {
        return;
    }
    it->second.entry.status = status;
    it->second.expires_at = now + ttl_;
}

ClientDedupCache::Shard& ClientDedupCache::ShardFor(const Key& key) { return shards_[KeyHash{}(key) % shards_.size()]; }

void ClientDedupCache::EvictExpiredLocked(Shard& shard, std::chrono::steady_clock::time_point now) {
    if (now < shard.next_evict_at && shard.entries.size() <= max_entries_per_shard_) {
        return;
    }

    for (auto it = shard.entries.begin(); it != shard.entries.end();) {
        if (it->second.expires_at <= now) {
            it = shard.entries.erase(it);
        } else {
            ++it;
        }
    }

    if (shard.entries.size() > max_entries_per_shard_) {
        std::vector<Key> keys;
        keys.reserve(shard.entries.size());
        for (const auto& [key, _] : shard.entries) {
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end(), [&shard](const Key& lhs, const Key& rhs) {
            return shard.entries.at(lhs).expires_at < shard.entries.at(rhs).expires_at;
        });
        const size_t target_size = max_entries_per_shard_ * 9 / 10;
        for (size_t i = 0; shard.entries.size() > target_size && i < keys.size(); ++i) {
            shard.entries.erase(keys[i]);
        }
    }

    shard.next_evict_at = now + std::chrono::seconds(5);
}
