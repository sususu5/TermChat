#include "msg_service.h"
#include <ctime>
#include "../dao/async_msg_writer.h"
#include "../log/log.h"
#include "../utils/id_generator.h"

MsgService::MsgService(PushService* push_service) : push_service_(push_service) {
    AsyncMsgWriter::GetInstance()->Start();
}

MsgService::~MsgService() { AsyncMsgWriter::GetInstance()->Stop(); }

void MsgService::OnMessagePersisted(uint64_t sender_id, uint64_t client_msg_id, const im::P2PMessage& msg,
                                    bool success) {
    if (!success) {
        LOG_ERROR("P2P Message[{}] persist failed after async retries.", msg.msg_id());
        return;
    }

    if (client_msg_id != 0) {
        msg_scylla_dao_.UpsertClientMsgDedup(sender_id, client_msg_id, msg.msg_id(), msg.receiver_id(),
                                             im::ACK_STATUS_PERSISTED);
    }

    if (push_service_) {
        im::MessageAck ack;
        ack.set_msg_id(msg.msg_id());
        ack.set_success(true);
        ack.set_status(im::ACK_STATUS_PERSISTED);
        ack.set_sender_id(sender_id);
        ack.set_receiver_id(msg.receiver_id());
        push_service_->push_message_ack(sender_id, ack);
    }
}

void MsgService::send_p2p_message(uint64_t sender_id, const im::P2PMessage& req, im::MessageAck* resp) {
    if (sender_id == 0) {
        resp->set_success(false);
        resp->set_error_msg("Sender ID is empty");
        resp->set_status(im::ACK_STATUS_UNKNOWN);
        return;
    }
    if (req.receiver_id() == 0) {
        resp->set_success(false);
        resp->set_error_msg("Receiver ID is empty");
        resp->set_status(im::ACK_STATUS_UNKNOWN);
        return;
    }
    if (req.timestamp() == 0) {
        resp->set_success(false);
        resp->set_error_msg("Timestamp is empty");
        resp->set_status(im::ACK_STATUS_UNKNOWN);
        return;
    }

    const auto client_msg_id = req.client_msg_id();
    if (client_msg_id != 0) {
        auto dedup = msg_scylla_dao_.GetClientMsgDedup(sender_id, client_msg_id);
        if (dedup.found) {
            resp->set_msg_id(dedup.server_msg_id);
            resp->set_success(true);
            resp->set_status(dedup.status);
            LOG_INFO("Deduplicated P2P retry: client_msg_id={}, msg_id={}", client_msg_id, dedup.server_msg_id);
            return;
        }
    }

    auto msg_to_store = req;
    msg_to_store.set_msg_id(IdGenerator::GenerateMsgId());
    msg_to_store.set_sender_id(sender_id);

    if (msg_to_store.timestamp() == 0) {
        msg_to_store.set_timestamp(time(nullptr));
    }

    if (client_msg_id != 0) {
        if (!msg_scylla_dao_.UpsertClientMsgDedup(sender_id, client_msg_id, msg_to_store.msg_id(), req.receiver_id(),
                                                  im::ACK_STATUS_RECEIVED)) {
            resp->set_success(false);
            resp->set_error_msg("Failed to persist message dedup state");
            resp->set_status(im::ACK_STATUS_UNKNOWN);
            return;
        }
    }

    AsyncMsgWriter::GetInstance()->Enqueue(msg_to_store,
                                           [this, sender_id, client_msg_id](const im::P2PMessage& msg, bool success) {
                                               OnMessagePersisted(sender_id, client_msg_id, msg, success);
                                           });

    const bool delivered = push_service_ && push_service_->push_p2p_message(msg_to_store);
    resp->set_msg_id(msg_to_store.msg_id());
    resp->set_success(true);
    resp->set_ref_seq(0);
    resp->set_status(delivered ? im::ACK_STATUS_ENQUEUED : im::ACK_STATUS_RECEIVED);
    resp->set_sender_id(sender_id);
    resp->set_receiver_id(req.receiver_id());

    if (client_msg_id != 0 && delivered) {
        msg_scylla_dao_.UpsertClientMsgDedup(sender_id, client_msg_id, msg_to_store.msg_id(), req.receiver_id(),
                                             im::ACK_STATUS_ENQUEUED);
    }

    LOG_INFO("P2P Message[{}] from User[{}] to User[{}] processed, ack_status={}.", msg_to_store.msg_id(), sender_id,
             req.receiver_id(), static_cast<int>(resp->status()));
}

void MsgService::acknowledge_message(uint64_t current_user_id, const im::MessageAck& req, im::MessageAck* resp) {
    if (req.msg_id() == 0 || req.sender_id() == 0 || req.receiver_id() == 0) {
        resp->set_success(false);
        resp->set_error_msg("Invalid message ack payload");
        resp->set_status(im::ACK_STATUS_UNKNOWN);
        return;
    }
    if (current_user_id != req.receiver_id()) {
        resp->set_success(false);
        resp->set_error_msg("Message ack receiver does not match current user");
        resp->set_status(im::ACK_STATUS_UNKNOWN);
        return;
    }
    if (req.status() != im::ACK_STATUS_DELIVERED && req.status() != im::ACK_STATUS_READ) {
        resp->set_success(false);
        resp->set_error_msg("Unsupported message ack status");
        resp->set_status(im::ACK_STATUS_UNKNOWN);
        return;
    }

    resp->set_msg_id(req.msg_id());
    resp->set_sender_id(req.sender_id());
    resp->set_receiver_id(req.receiver_id());
    resp->set_status(req.status());
    resp->set_success(true);

    if (push_service_) {
        push_service_->push_message_ack(req.sender_id(), *resp);
    }
    LOG_INFO("Message[{}] acked by receiver={}, sender={}, status={}.", req.msg_id(), req.receiver_id(),
             req.sender_id(), static_cast<int>(req.status()));
}

void MsgService::sync_messages(uint64_t user_id, const im::SyncMessagesReq& req, im::SyncMessagesResp* resp) {
    if (user_id == 0) {
        resp->set_success(false);
        resp->set_error_msg("User ID is empty");
        return;
    }

    auto page = msg_scylla_dao_.GetMessagesForUserAfter(user_id, req.last_ack_msg_id(), req.limit());

    resp->set_success(true);
    for (const auto& msg : page.messages) {
        *resp->add_messages() = msg;
    }
    resp->set_next_ack_msg_id(page.next_ack_msg_id);
    resp->set_has_more(page.has_more);

    LOG_INFO("User[{}] synced {} messages after msg_id={}, next_ack_msg_id={}, has_more={}.", user_id,
             page.messages.size(), req.last_ack_msg_id(), page.next_ack_msg_id, page.has_more);
}
