#include "msg_service.h"
#include <ctime>
#include <string>
#include "../dao/async_msg_writer.h"
#include "../log/log.h"
#include "../utils/id_generator.h"

MsgService::MsgService(PushService* push_service) : push_service_(push_service) {
    AsyncMsgWriter::GetInstance()->Start();
}

std::string MsgService::MakeClientMsgKey(uint64_t sender_id, uint64_t client_msg_id) const {
    return std::to_string(sender_id) + ":" + std::to_string(client_msg_id);
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

    if (req.client_msg_id() != 0) {
        const auto key = MakeClientMsgKey(sender_id, req.client_msg_id());
        std::lock_guard<std::mutex> lock(ack_cache_mutex_);
        if (ack_cache_.contains(key)) {
            resp->set_msg_id(ack_cache_[key].msg_id);
            resp->set_success(true);
            resp->set_status(ack_cache_[key].status);
            LOG_INFO("Deduplicated P2P retry: client_msg_id={}, msg_id={}", req.client_msg_id(),
                     ack_cache_[key].msg_id);
            return;
        }
    }

    auto msg_to_store = req;
    msg_to_store.set_msg_id(IdGenerator::GenerateMsgId());
    msg_to_store.set_sender_id(sender_id);

    if (msg_to_store.timestamp() == 0) {
        msg_to_store.set_timestamp(time(nullptr));
    }

    AsyncMsgWriter::GetInstance()->Enqueue(msg_to_store);

    const bool delivered = push_service_ && push_service_->push_p2p_message(msg_to_store);
    resp->set_msg_id(msg_to_store.msg_id());
    resp->set_success(true);
    resp->set_ref_seq(0);
    resp->set_status(delivered ? im::ACK_STATUS_DELIVERED : im::ACK_STATUS_RECEIVED);

    if (req.client_msg_id() != 0) {
        const auto key = MakeClientMsgKey(sender_id, req.client_msg_id());
        std::lock_guard<std::mutex> lock(ack_cache_mutex_);
        ack_cache_[key] = AckCacheEntry{msg_to_store.msg_id(), resp->status()};
    }

    LOG_INFO("P2P Message[{}] from User[{}] to User[{}] processed, ack_status={}.", msg_to_store.msg_id(), sender_id,
             req.receiver_id(), static_cast<int>(resp->status()));
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
