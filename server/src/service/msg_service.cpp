#include "msg_service.h"
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <ctime>
#include <string>
#include "../dao/async_msg_writer.h"
#include "../utils/id_generator.h"

namespace {
bool EnvFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value) {
        return false;
    }
    const std::string flag(value);
    return flag == "1" || flag == "true" || flag == "TRUE" || flag == "on" || flag == "ON";
}

void FillDedupAck(uint64_t sender_id, const ClientDedupCache::Entry& entry, im::MessageAck* resp) {
    resp->set_msg_id(entry.server_msg_id);
    resp->set_success(true);
    resp->set_status(entry.status);
    resp->set_sender_id(sender_id);
    resp->set_receiver_id(entry.receiver_id);
}
}  // namespace

MsgService::MsgService(PushService* push_service) : push_service_(push_service) {
    push_persisted_ack_ = EnvFlagEnabled("TERMCHAT_PUSH_PERSISTED_ACK") || EnvFlagEnabled("ENABLE_PERSISTED_ACK_PUSH");
    const bool disable_sync_dedup = EnvFlagEnabled("TERMCHAT_DISABLE_SYNC_CLIENT_DEDUP");
    fast_client_dedup_ = !disable_sync_dedup && EnvFlagEnabled("TERMCHAT_FAST_CLIENT_DEDUP");
    sync_client_dedup_ = !disable_sync_dedup && !fast_client_dedup_;
    AsyncMsgWriter::GetInstance()->Start();
    spdlog::info("MsgService persisted ACK push: {}", push_persisted_ack_ ? "enabled" : "disabled");
    spdlog::info("MsgService synchronous client dedup: {}", sync_client_dedup_ ? "enabled" : "disabled");
    spdlog::info("MsgService fast client dedup cache: {}", fast_client_dedup_ ? "enabled" : "disabled");
}

MsgService::~MsgService() { AsyncMsgWriter::GetInstance()->Stop(); }

void MsgService::OnMessagePersisted(uint64_t sender_id, uint64_t client_msg_id, const im::P2PMessage& msg,
                                    bool success) {
    if (!success) {
        spdlog::error("P2P Message[{}] persist failed after async retries.", msg.msg_id());
        return;
    }

    if (fast_client_dedup_ && client_msg_id != 0) {
        client_dedup_cache_.UpdateStatus(sender_id, client_msg_id, msg.msg_id(), im::ACK_STATUS_PERSISTED);
    }

    if (client_msg_id != 0) {
        msg_scylla_dao_.UpsertClientMsgDedup(sender_id, client_msg_id, msg.msg_id(), msg.receiver_id(),
                                             im::ACK_STATUS_PERSISTED);
    }

    if (push_persisted_ack_ && push_service_) {
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
    if (sync_client_dedup_ && client_msg_id != 0) {
        auto dedup = msg_scylla_dao_.GetClientMsgDedup(sender_id, client_msg_id);
        if (dedup.found) {
            resp->set_msg_id(dedup.server_msg_id);
            resp->set_success(true);
            resp->set_status(dedup.status);
            spdlog::info("Deduplicated P2P retry: client_msg_id={}, msg_id={}", client_msg_id, dedup.server_msg_id);
            return;
        }
    }

    auto msg_to_store = req;
    msg_to_store.set_msg_id(IdGenerator::GenerateMsgId());
    msg_to_store.set_sender_id(sender_id);

    if (msg_to_store.timestamp() == 0) {
        msg_to_store.set_timestamp(time(nullptr));
    }

    if (fast_client_dedup_ && client_msg_id != 0) {
        const auto reserve = client_dedup_cache_.Reserve(sender_id, client_msg_id, msg_to_store.msg_id(),
                                                         req.receiver_id(), im::ACK_STATUS_RECEIVED);
        if (reserve.state == ClientDedupCache::ReserveState::kDuplicate) {
            FillDedupAck(sender_id, reserve.entry, resp);
            spdlog::info("Fast deduplicated P2P retry: client_msg_id={}, msg_id={}", client_msg_id,
                         reserve.entry.server_msg_id);
            return;
        }
        if (reserve.state == ClientDedupCache::ReserveState::kConflict) {
            resp->set_success(false);
            resp->set_error_msg("Conflicting duplicate client_msg_id");
            resp->set_status(im::ACK_STATUS_UNKNOWN);
            return;
        }
    }

    if (sync_client_dedup_ && client_msg_id != 0) {
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

    if (fast_client_dedup_ && client_msg_id != 0) {
        client_dedup_cache_.UpdateStatus(sender_id, client_msg_id, msg_to_store.msg_id(),
                                         delivered ? im::ACK_STATUS_ENQUEUED : im::ACK_STATUS_RECEIVED);
    }

    if (sync_client_dedup_ && client_msg_id != 0 && delivered) {
        msg_scylla_dao_.UpsertClientMsgDedup(sender_id, client_msg_id, msg_to_store.msg_id(), req.receiver_id(),
                                             im::ACK_STATUS_ENQUEUED);
    }

    spdlog::info("P2P Message[{}] from User[{}] to User[{}] processed, ack_status={}.", msg_to_store.msg_id(),
                 sender_id, req.receiver_id(), static_cast<int>(resp->status()));
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
    spdlog::info("Message[{}] acked by receiver={}, sender={}, status={}.", req.msg_id(), req.receiver_id(),
                 req.sender_id(), static_cast<int>(req.status()));
}

void MsgService::acknowledge_message_batch(uint64_t current_user_id, const im::MessageAckBatch& req,
                                           im::MessageAckBatch* resp) {
    for (const auto& ack : req.acks()) {
        im::MessageAck* ack_resp = resp->add_acks();
        acknowledge_message(current_user_id, ack, ack_resp);
    }
    spdlog::info("User[{}] acknowledged {} messages in batch.", current_user_id, req.acks_size());
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

    spdlog::info("User[{}] synced {} messages after msg_id={}, next_ack_msg_id={}, has_more={}.", user_id,
                 page.messages.size(), req.last_ack_msg_id(), page.next_ack_msg_id, page.has_more);
}
