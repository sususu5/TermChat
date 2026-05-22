#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include "../dao/msg_scylla_dao.h"
#include "message_service.pb.h"
#include "push_service.h"

class MsgService {
public:
    explicit MsgService(PushService* push_service);
    ~MsgService() = default;

    // Send a P2P message
    void send_p2p_message(uint64_t sender_id, const im::P2PMessage& req, im::MessageAck* resp);
    // Sync offline messages
    void sync_messages(uint64_t user_id, const im::SyncMessagesReq& req, im::SyncMessagesResp* resp);

private:
    std::string MakeClientMsgKey(uint64_t sender_id, uint64_t client_msg_id) const;

    struct AckCacheEntry {
        uint64_t msg_id = 0;
        im::MessageAckStatus status = im::ACK_STATUS_UNKNOWN;
    };

    PushService* push_service_;
    MsgScyllaDao msg_scylla_dao_;
    std::mutex ack_cache_mutex_;
    std::unordered_map<std::string, AckCacheEntry> ack_cache_;
};
