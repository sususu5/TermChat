#pragma once

#include <cstdint>
#include "../dao/msg_scylla_dao.h"
#include "message_service.pb.h"
#include "push_service.h"

class MsgService {
public:
    explicit MsgService(PushService* push_service);
    ~MsgService();

    // Send a P2P message
    void send_p2p_message(uint64_t sender_id, const im::P2PMessage& req, im::MessageAck* resp);
    void acknowledge_message(uint64_t current_user_id, const im::MessageAck& req, im::MessageAck* resp);
    void acknowledge_message_batch(uint64_t current_user_id, const im::MessageAckBatch& req, im::MessageAckBatch* resp);
    // Sync offline messages
    void sync_messages(uint64_t user_id, const im::SyncMessagesReq& req, im::SyncMessagesResp* resp);

private:
    void OnMessagePersisted(uint64_t sender_id, uint64_t client_msg_id, const im::P2PMessage& msg, bool success);

    PushService* push_service_;
    MsgScyllaDao msg_scylla_dao_;
    bool push_persisted_ack_ = false;
    bool sync_client_dedup_ = true;
};
