#pragma once

#include <cstdint>
#include <vector>
#include "message_service.pb.h"

class MsgScyllaDao {
public:
    struct MessagePage {
        std::vector<im::P2PMessage> messages;
        uint64_t next_ack_msg_id = 0;
        bool has_more = false;
    };

    struct ClientMsgDedupEntry {
        bool found = false;
        uint64_t server_msg_id = 0;
        uint64_t receiver_id = 0;
        im::MessageAckStatus status = im::ACK_STATUS_UNKNOWN;
    };

    MsgScyllaDao() = default;
    ~MsgScyllaDao() = default;

    bool InsertMessage(const im::P2PMessage& msg);
    bool InsertBatch(const std::vector<im::P2PMessage>& msgs);
    MessagePage GetMessagesForUserAfter(uint64_t user_id, uint64_t last_ack_msg_id, uint32_t limit);
    ClientMsgDedupEntry GetClientMsgDedup(uint64_t sender_id, uint64_t client_msg_id);
    bool UpsertClientMsgDedup(uint64_t sender_id, uint64_t client_msg_id, uint64_t server_msg_id, uint64_t receiver_id,
                              im::MessageAckStatus status);
};
