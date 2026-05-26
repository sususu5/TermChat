#include "protobuf_handler.h"
#include <arpa/inet.h>

ProtobufHandler::ProtobufHandler(TcpConnection* conn, AuthService* auth_service, FriendService* friend_service,
                                 MsgService* msg_service, ThreadPool* thread_pool)
    : conn_(conn), auth_service_(auth_service), friend_service_(friend_service), msg_service_(msg_service),
      thread_pool_(thread_pool) {}

bool ProtobufHandler::Process(Buffer& read_buff, Buffer& write_buff) {
    im::Envelope request;

    if (!TryDecodeMessage(read_buff, request)) {
        return false;
    }

    LOG_DEBUG("Received message: cmd={}, seq={}", request.cmd(), request.seq());

    im::Envelope response;
    response.set_seq(request.seq());
    response.set_timestamp(time(nullptr));
    Dispatch(request, response);
    EncodeMessage(response, write_buff);

    LOG_DEBUG("Sent response: cmd={}, seq={}", response.cmd(), response.seq());
    return true;
}

bool ProtobufHandler::TryDecodeMessage(Buffer& read_buff, im::Envelope& envelope) {
    if (read_buff.readable_bytes() < kHeaderSize) {
        return false;
    }

    uint32_t msg_len = 0;
    memcpy(&msg_len, read_buff.peek(), kHeaderSize);
    msg_len = ntohl(msg_len);

    if (msg_len > kMaxMessageSize) {
        LOG_ERROR("Message too large: {} bytes (max: {})", msg_len, kMaxMessageSize);
        read_buff.retrieve(kHeaderSize);
        return false;
    }

    if (read_buff.readable_bytes() < kHeaderSize + msg_len) {
        return false;
    }

    // Parse the protobuf message
    const char* payload = read_buff.peek() + kHeaderSize;
    if (!envelope.ParseFromArray(payload, msg_len)) {
        LOG_ERROR("Failed to parse protobuf message");
        read_buff.retrieve(kHeaderSize + msg_len);
        return false;
    }

    // Consume the message from buffer
    read_buff.retrieve(kHeaderSize + msg_len);
    return true;
}

void ProtobufHandler::EncodeMessage(const im::Envelope& envelope, Buffer& write_buff) {
    std::string serialized;
    if (!envelope.SerializeToString(&serialized)) {
        LOG_ERROR("Failed to serialize protobuf message");
        return;
    }

    uint32_t msg_len = htonl(static_cast<uint32_t>(serialized.size()));
    write_buff.append(&msg_len, kHeaderSize);
    write_buff.append(serialized.data(), serialized.size());
}

void ProtobufHandler::Dispatch(const im::Envelope& request, im::Envelope& response) {
    switch (request.cmd()) {
        case im::CMD_REGISTER_REQ:
            HandleRegister(request, response);
            break;
        case im::CMD_LOGIN_REQ:
            HandleLogin(request, response);
            break;
        case im::CMD_ADD_FRIEND_REQ:
            HandleAddFriend(request, response);
            break;
        case im::CMD_HANDLE_FRIEND_REQ:
            HandleHandleFriend(request, response);
            break;
        case im::CMD_GET_FRIEND_LIST_REQ:
            HandleGetFriendList(request, response);
            break;
        case im::CMD_P2P_MSG_REQ:
            HandleP2PMsg(request, response);
            break;
        case im::CMD_MSG_ACK:
            HandleMessageAck(request, response);
            break;
        case im::CMD_MSG_ACK_BATCH:
            HandleMessageAckBatch(request, response);
            break;
        case im::CMD_SYNC_MSGS_REQ:
            HandleSyncMessages(request, response);
            break;
        case im::CMD_HEARTBEAT:
            // Heartbeat received, connection timer is already refreshed by OnRead_
            return;
        default:
            HandleUnknown(request, response);
            break;
    }
}

void ProtobufHandler::HandleRegister(const im::Envelope& request, im::Envelope& response) {
    if (!request.has_register_req()) {
        LOG_ERROR("CMD_REGISTER_REQ received but payload is missing");
        response.set_cmd(im::CMD_REGISTER_RES);
        auto* resp = response.mutable_register_res();
        resp->set_success(false);
        resp->set_error_msg("Invalid request: missing register payload");
        return;
    }

    const auto& req = request.register_req();
    LOG_INFO("Register request: username={}", req.username());

    im::RegisterResp register_resp;
    auth_service_->user_register(req, &register_resp);
    response.set_cmd(im::CMD_REGISTER_RES);
    response.mutable_register_res()->CopyFrom(register_resp);
}

void ProtobufHandler::HandleLogin(const im::Envelope& request, im::Envelope& response) {
    if (!request.has_login_req()) {
        LOG_ERROR("CMD_LOGIN_REQ received but payload is missing");
        response.set_cmd(im::CMD_LOGIN_RES);
        auto* resp = response.mutable_login_res();
        resp->set_success(false);
        resp->set_error_msg("Invalid request: missing login payload");
        return;
    }

    const auto& req = request.login_req();
    LOG_INFO("Login request: username={}", req.username());

    im::LoginResp login_resp;
    auth_service_->user_login(conn_, req, &login_resp);
    response.set_cmd(im::CMD_LOGIN_RES);
    response.mutable_login_res()->CopyFrom(login_resp);

    if (login_resp.success()) {
        auto user_id = login_resp.user_info().user_id();
        auto* friend_svc = friend_service_;
        thread_pool_->AddTask([user_id, friend_svc]() {
            auto pending_reqs = friend_svc->GetPendingRequests(user_id);
            if (pending_reqs.empty()) return;
            if (TcpConnection::push_service) {
                for (const auto& req : pending_reqs) {
                    im::Envelope env;
                    env.set_cmd(im::CMD_FRIEND_REQ_PUSH);
                    env.set_seq(0);
                    env.set_timestamp(time(nullptr));
                    auto* payload = env.mutable_friend_req_push();
                    *payload = req;
                    std::string serialized;
                    if (env.SerializeToString(&serialized)) {
                        TcpConnection::push_service->push_to_user(user_id, std::move(serialized));
                        LOG_INFO("Pushed {} pending friend requests to user: {}", pending_reqs.size(), user_id);
                    } else {
                        LOG_ERROR("Failed to serialize friend request push message");
                    }
                }
            }
        });
    }
}

void ProtobufHandler::HandleAddFriend(const im::Envelope& request, im::Envelope& response) {
    if (RequireAuth(response, im::CMD_ADD_FRIEND_RES)) return;

    if (!request.has_add_friend_req()) {
        LOG_ERROR("CMD_ADD_FRIEND_REQ received but payload is missing");
        response.set_cmd(im::CMD_ADD_FRIEND_RES);
        auto* resp = response.mutable_add_friend_res();
        resp->set_success(false);
        resp->set_error_msg("Invalid request: missing add friend payload");
        return;
    }

    const auto& req = request.add_friend_req();
    LOG_INFO("Add friend request: sender={}, receiver_id={}", CurrentUserId(), req.receiver_id());

    im::AddFriendResp add_friend_resp;
    friend_service_->AddFriend(CurrentUserId(), req, &add_friend_resp);
    response.set_cmd(im::CMD_ADD_FRIEND_RES);
    response.mutable_add_friend_res()->CopyFrom(add_friend_resp);
}

void ProtobufHandler::HandleHandleFriend(const im::Envelope& request, im::Envelope& response) {
    if (RequireAuth(response, im::CMD_HANDLE_FRIEND_RES)) return;

    if (!request.has_handle_friend_req()) {
        LOG_ERROR("CMD_HANDLE_FRIEND_REQ received but payload is missing");
        response.set_cmd(im::CMD_HANDLE_FRIEND_RES);
        auto* resp = response.mutable_handle_friend_res();
        resp->set_success(false);
        resp->set_error_msg("Invalid request: missing handle friend payload");
        return;
    }

    const auto& req = request.handle_friend_req();
    LOG_INFO("Handle friend request: receiver={}, req_id={}, sender_id={}", CurrentUserId(), req.req_id(),
             req.sender_id());

    im::HandleFriendResp handle_friend_resp;
    friend_service_->HandleFriend(CurrentUserId(), req, &handle_friend_resp);
    response.set_cmd(im::CMD_HANDLE_FRIEND_RES);
    response.mutable_handle_friend_res()->CopyFrom(handle_friend_resp);
}

void ProtobufHandler::HandleGetFriendList(const im::Envelope& request, im::Envelope& response) {
    if (RequireAuth(response, im::CMD_GET_FRIEND_LIST_RES)) return;

    if (!request.has_get_friend_list_req()) {
        LOG_ERROR("CMD_GET_FRIEND_LIST_REQ received but payload is missing");
        response.set_cmd(im::CMD_GET_FRIEND_LIST_RES);
        auto* resp = response.mutable_get_friend_list_res();
        resp->set_success(false);
        resp->set_error_msg("Invalid request: missing get friend list payload");
        return;
    }

    LOG_INFO("Get friend list request: user={}", CurrentUserId());

    im::GetFriendListResp get_friend_list_resp;
    friend_service_->GetFriendList(CurrentUserId(), &get_friend_list_resp);
    response.set_cmd(im::CMD_GET_FRIEND_LIST_RES);
    response.mutable_get_friend_list_res()->CopyFrom(get_friend_list_resp);
}

void ProtobufHandler::HandleP2PMsg(const im::Envelope& request, im::Envelope& response) {
    if (RequireAuth(response, im::CMD_MSG_ACK)) return;

    if (!request.has_p2p_msg_req()) {
        LOG_ERROR("CMD_P2P_MSG_REQ received but payload is missing");
        response.set_cmd(im::CMD_MSG_ACK);
        auto* resp = response.mutable_msg_ack();
        resp->set_success(false);
        resp->set_error_msg("Invalid request: missing p2p message payload");
        resp->set_status(im::ACK_STATUS_UNKNOWN);
        return;
    }

    const auto& req = request.p2p_msg_req();
    LOG_INFO("P2P Msg request: from={} to={}", CurrentUserId(), req.receiver_id());

    im::MessageAck msg_ack;
    msg_service_->send_p2p_message(CurrentUserId(), req, &msg_ack);
    msg_ack.set_ref_seq(request.seq());
    response.set_cmd(im::CMD_MSG_ACK);
    response.mutable_msg_ack()->CopyFrom(msg_ack);
}

void ProtobufHandler::HandleMessageAck(const im::Envelope& request, im::Envelope& response) {
    if (RequireAuth(response, im::CMD_MSG_ACK)) return;

    response.set_cmd(im::CMD_MSG_ACK);
    if (!request.has_msg_ack()) {
        auto* resp = response.mutable_msg_ack();
        resp->set_success(false);
        resp->set_error_msg("Invalid request: missing message ack payload");
        resp->set_status(im::ACK_STATUS_UNKNOWN);
        return;
    }

    im::MessageAck ack_resp;
    msg_service_->acknowledge_message(CurrentUserId(), request.msg_ack(), &ack_resp);
    ack_resp.set_ref_seq(request.seq());
    response.mutable_msg_ack()->CopyFrom(ack_resp);
}

void ProtobufHandler::HandleMessageAckBatch(const im::Envelope& request, im::Envelope& response) {
    if (RequireAuth(response, im::CMD_MSG_ACK_BATCH)) return;

    response.set_cmd(im::CMD_MSG_ACK_BATCH);
    if (!request.has_msg_ack_batch()) {
        auto* ack_resp = response.mutable_msg_ack_batch()->add_acks();
        ack_resp->set_success(false);
        ack_resp->set_error_msg("Invalid request: missing message ack batch payload");
        ack_resp->set_status(im::ACK_STATUS_UNKNOWN);
        return;
    }

    im::MessageAckBatch ack_batch_resp;
    msg_service_->acknowledge_message_batch(CurrentUserId(), request.msg_ack_batch(), &ack_batch_resp);
    for (auto& ack_resp : *ack_batch_resp.mutable_acks()) {
        ack_resp.set_ref_seq(request.seq());
    }
    response.mutable_msg_ack_batch()->CopyFrom(ack_batch_resp);
}

void ProtobufHandler::HandleSyncMessages(const im::Envelope& request, im::Envelope& response) {
    if (RequireAuth(response, im::CMD_SYNC_MSGS_RES)) return;

    if (!request.has_sync_msgs_req()) {
        LOG_ERROR("CMD_SYNC_MSGS_REQ received but payload is missing");
        response.set_cmd(im::CMD_SYNC_MSGS_RES);
        auto* resp = response.mutable_sync_msgs_res();
        resp->set_success(false);
        resp->set_error_msg("Invalid request: missing sync messages payload");
        return;
    }

    const auto& req = request.sync_msgs_req();
    LOG_INFO("Sync messages request: user={}", CurrentUserId());

    im::SyncMessagesResp sync_resp;
    msg_service_->sync_messages(CurrentUserId(), req, &sync_resp);
    response.set_cmd(im::CMD_SYNC_MSGS_RES);
    response.mutable_sync_msgs_res()->CopyFrom(sync_resp);
}

void ProtobufHandler::HandleUnknown(const im::Envelope& request, im::Envelope& response) {
    LOG_WARN("Unknown command received: {}", request.cmd());
    response.set_cmd(im::CMD_UNKNOWN);
}

bool ProtobufHandler::RequireAuth(im::Envelope& response, im::CommandType resp_cmd) {
    if (!conn_ || !conn_->is_logged_in()) {
        LOG_WARN("Unauthorized request: user not logged in");
        response.set_cmd(resp_cmd);
        if (resp_cmd == im::CMD_MSG_ACK) {
            auto* ack = response.mutable_msg_ack();
            ack->set_success(false);
            ack->set_error_msg("Unauthorized: user not logged in");
            ack->set_status(im::ACK_STATUS_UNKNOWN);
        }
        return true;
    }
    return false;
}

uint64_t ProtobufHandler::CurrentUserId() const { return conn_ ? conn_->get_user_id() : 0; }
