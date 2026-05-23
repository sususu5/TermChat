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

    MsgScyllaDao() = default;
    ~MsgScyllaDao() = default;

    bool InsertMessage(const im::P2PMessage& msg);
    bool InsertBatch(const std::vector<im::P2PMessage>& msgs);
    MessagePage GetMessagesForUserAfter(uint64_t user_id, uint64_t last_ack_msg_id, uint32_t limit);
};
