#include "msg_service.h"
#include <ctime>
#include "../dao/async_msg_writer.h"
#include "../log/log.h"
#include "../utils/id_generator.h"

MsgService::MsgService(PushService* push_service) : push_service_(push_service) {
    AsyncMsgWriter::GetInstance()->Start();
}

void MsgService::send_p2p_message(uint64_t sender_id, const im::P2PMessage& req, im::MessageAck* resp) {
    if (sender_id == 0) {
        resp->set_success(false);
        resp->set_error_msg("Sender ID is empty");
        return;
    }
    if (req.receiver_id() == 0) {
        resp->set_success(false);
        resp->set_error_msg("Receiver ID is empty");
        return;
    }
    if (req.timestamp() == 0) {
        resp->set_success(false);
        resp->set_error_msg("Timestamp is empty");
        return;
    }

    auto msg_to_store = req;
    msg_to_store.set_msg_id(IdGenerator::GenerateMsgId());
    msg_to_store.set_sender_id(sender_id);

    if (msg_to_store.timestamp() == 0) {
        msg_to_store.set_timestamp(time(nullptr));
    }

    AsyncMsgWriter::GetInstance()->Enqueue(msg_to_store);

    if (push_service_) {
        push_service_->push_p2p_message(msg_to_store);
    }

    resp->set_msg_id(msg_to_store.msg_id());
    resp->set_success(true);
    resp->set_ref_seq(0);

    LOG_INFO("P2P Message[{}] from User[{}] to User[{}] processed.", msg_to_store.msg_id(), sender_id,
             req.receiver_id());
}

void MsgService::sync_messages(uint64_t user_id, const im::SyncMessagesReq& req, im::SyncMessagesResp* resp) {
    if (user_id == 0) {
        resp->set_success(false);
        resp->set_error_msg("User ID is empty");
        return;
    }

    auto messages = msg_scylla_dao_.GetMessagesForUser(user_id);

    resp->set_success(true);
    for (const auto& msg : messages) {
        *resp->add_messages() = msg;
    }

    LOG_INFO("User[{}] synced {} messages (latest 500).", user_id, messages.size());
}
