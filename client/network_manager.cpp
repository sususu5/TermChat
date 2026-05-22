#include "network_manager.h"
#include <algorithm>
#include <chrono>

bool NetworkManager::Connect(const std::string& host, int port) {
    if (connected_) return true;

    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        return false;
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr) <= 0) {
        return false;
    }

    if (connect(sock_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        return false;
    }

    connected_ = true;
    running_ = true;
    listener_thread_ = std::thread(&NetworkManager::ListenerLoop, this);
    heartbeat_thread_ = std::thread(&NetworkManager::HeartbeatLoop, this);
    return true;
}

void NetworkManager::Disconnect() {
    running_ = false;
    if (sock_ != -1) {
        shutdown(sock_, SHUT_RDWR);
        close(sock_);
        sock_ = -1;
    }

    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    connected_ = false;
}

bool NetworkManager::SendEnvelope(const im::Envelope& env) {
    if (!connected_) return false;

    auto serialized = env.SerializeAsString();
    auto len = htonl(static_cast<uint32_t>(serialized.size()));

    std::string packet;
    packet.append(reinterpret_cast<char*>(&len), 4);
    packet.append(serialized);

    auto sent = send(sock_, packet.data(), packet.size(), 0);
    return sent == static_cast<ssize_t>(packet.size());
}

uint64_t NetworkManager::NextSeq() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_seq_++;
}

uint64_t NetworkManager::GenerateClientMsgId() {
    const auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    return (static_cast<uint64_t>(now_ms) << 20) | (NextSeq() & 0xFFFFFULL);
}

bool NetworkManager::WaitForResponse(uint64_t seq, im::Envelope& response, im::CommandType expected_cmd,
                                     std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = cv_response_.wait_for(lock, timeout, [this, seq] { return response_by_seq_.contains(seq); });
    if (!ready) {
        return false;
    }

    auto it = response_by_seq_.find(seq);
    if (it == response_by_seq_.end()) {
        return false;
    }

    response = std::move(it->second);
    response_by_seq_.erase(it);
    return response.cmd() == expected_cmd;
}

bool NetworkManager::SendRequestAndWait(im::Envelope request, im::Envelope& response, im::CommandType expected_cmd) {
    if (request.seq() == 0) {
        request.set_seq(NextSeq());
    }

    if (!SendEnvelope(request)) {
        return false;
    }

    return WaitForResponse(request.seq(), response, expected_cmd, std::chrono::seconds(5));
}

void NetworkManager::ListenerLoop() {
    while (running_) {
        uint32_t net_len;
        auto len_buf = reinterpret_cast<char*>(&net_len);
        size_t received = 0;

        // Read header
        while (received < 4 && running_) {
            auto r = recv(sock_, len_buf + received, 4 - received, 0);
            if (r <= 0) {
                if (running_) {
                    if (on_error_callback_) on_error_callback_("Connection lost");
                    running_ = false;
                    connected_ = false;
                }
                return;
            }
            received += r;
        }
        if (!running_) break;

        auto msg_len = ntohl(net_len);
        if (msg_len > 10 * 1024 * 1024) {  // 10MB limit
            if (running_) {
                if (on_error_callback_) on_error_callback_("Protocol Error: Packet too large");
                running_ = false;
                connected_ = false;
            }
            return;
        }

        std::string buffer;
        buffer.resize(msg_len);
        received = 0;

        // Read body
        while (received < msg_len && running_) {
            auto r = recv(sock_, &buffer[received], msg_len - received, 0);
            if (r <= 0) {
                if (running_) {
                    if (on_error_callback_) on_error_callback_("Connection lost reading body");
                    running_ = false;
                    connected_ = false;
                }
                return;
            }
            received += r;
        }
        if (!running_) break;

        im::Envelope env;
        if (!env.ParseFromString(buffer)) {
            continue;
        }

        // Handle Packet
        if (env.cmd() == im::CMD_FRIEND_REQ_PUSH) {
            const auto req = env.friend_req_push();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_friend_requests_.push_back(req);
            }
            if (on_friend_request_callback_) {
                on_friend_request_callback_(req);
            }
        } else if (env.cmd() == im::CMD_FRIEND_STATUS_PUSH) {
            if (on_friend_status_callback_) {
                on_friend_status_callback_(env.friend_status_push());
            }
        } else if (env.cmd() == im::CMD_P2P_MSG_PUSH) {
            auto msg = env.p2p_msg_push();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                p2p_chat_history_[msg.sender_id()].push_back(msg);
            }
            if (on_message_callback_) {
                on_message_callback_(msg);
            }
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            response_by_seq_[env.seq()] = env;
            // If the command is not push, notify the response
            cv_response_.notify_one();
        }
    }
}

void NetworkManager::HeartbeatLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        if (!running_) break;
        if (connected_) {
            im::Envelope env;
            env.set_cmd(im::CMD_HEARTBEAT);
            env.set_timestamp(time(NULL));
            SendEnvelope(env);
        }
    }
}

bool NetworkManager::Register(const std::string& username, const std::string& password, std::string& error_msg) {
    im::RegisterReq req;
    req.set_username(username);
    req.set_password(password);

    im::Envelope env;
    env.set_cmd(im::CMD_REGISTER_REQ);
    env.set_timestamp(time(NULL));
    *env.mutable_register_req() = req;

    im::Envelope resp_env;
    if (!SendRequestAndWait(env, resp_env, im::CMD_REGISTER_RES)) {
        error_msg = "Request timeout or network error";
        return false;
    }

    const auto& resp = resp_env.register_res();
    if (resp.success()) {
        user_id_ = resp.user_id();
        return true;
    } else {
        error_msg = resp.error_msg();
        return false;
    }
}

bool NetworkManager::Login(const std::string& username, const std::string& password, std::string& error_msg) {
    im::LoginReq req;
    req.set_username(username);
    req.set_password(password);

    im::Envelope env;
    env.set_cmd(im::CMD_LOGIN_REQ);
    env.set_timestamp(time(NULL));
    *env.mutable_login_req() = req;

    im::Envelope resp_env;
    if (!SendRequestAndWait(env, resp_env, im::CMD_LOGIN_RES)) {
        error_msg = "Request timeout or network error";
        return false;
    }

    const auto& resp = resp_env.login_res();
    if (resp.success()) {
        token_ = resp.token();
        if (resp.has_user_info()) {
            user_id_ = resp.user_info().user_id();
            username_ = resp.user_info().username();
        }
        return true;
    } else {
        error_msg = resp.error_msg();
        return false;
    }
}

bool NetworkManager::Logout(std::string& error_msg) {
    if (!IsLoggedIn()) {
        return true;
    }
    ClearAuth();
    Disconnect();
    return true;
}

void NetworkManager::ClearAuth() {
    token_.clear();
    user_id_ = 0;
    username_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        p2p_chat_history_.clear();
        pending_p2p_messages_.clear();
        response_by_seq_.clear();
        pending_friend_requests_.clear();
    }
}

bool NetworkManager::AddFriend(uint64_t receiver_id, const std::string& verify_msg, std::string& error_msg) {
    im::AddFriendReq req;
    req.set_receiver_id(receiver_id);
    req.set_verify_msg(verify_msg);

    im::Envelope env;
    env.set_cmd(im::CMD_ADD_FRIEND_REQ);
    env.set_timestamp(time(NULL));
    *env.mutable_add_friend_req() = req;

    im::Envelope resp_env;
    if (!SendRequestAndWait(env, resp_env, im::CMD_ADD_FRIEND_RES)) {
        error_msg = "Request timeout or network error";
        return false;
    }

    const auto& resp = resp_env.add_friend_res();
    if (resp.success()) {
        return true;
    } else {
        error_msg = resp.error_msg();
        return false;
    }
}

bool NetworkManager::HandleFriendRequest(uint64_t req_id, uint64_t sender_id, im::FriendAction action,
                                         std::string& error_msg) {
    im::HandleFriendReq req;
    req.set_req_id(req_id);
    req.set_sender_id(sender_id);
    req.set_action(action);

    im::Envelope env;
    env.set_cmd(im::CMD_HANDLE_FRIEND_REQ);
    env.set_timestamp(time(NULL));
    *env.mutable_handle_friend_req() = req;

    im::Envelope resp_env;
    if (!SendRequestAndWait(env, resp_env, im::CMD_HANDLE_FRIEND_RES)) {
        error_msg = "Request timeout or network error";
        return false;
    }

    const auto& resp = resp_env.handle_friend_res();
    if (resp.success()) {
        return true;
    } else {
        error_msg = resp.error_msg();
        return false;
    }
}

bool NetworkManager::GetFriendList(std::vector<im::User>& friend_info_list, std::string& error_msg) {
    im::GetFriendListReq req;

    im::Envelope env;
    env.set_cmd(im::CMD_GET_FRIEND_LIST_REQ);
    env.set_timestamp(time(NULL));
    *env.mutable_get_friend_list_req() = req;

    im::Envelope resp_env;
    if (!SendRequestAndWait(env, resp_env, im::CMD_GET_FRIEND_LIST_RES)) {
        error_msg = "Request timeout or network error";
        return false;
    }

    auto* resp = resp_env.mutable_get_friend_list_res();
    if (resp->success()) {
        friend_info_list.clear();
        friend_info_list.reserve(resp->friend_list_size());
        for (auto& user : *resp->mutable_friend_list()) {
            friend_info_list.push_back(std::move(user));
        }
        return true;
    } else {
        error_msg = resp->error_msg();
        return false;
    }
}

std::vector<im::FriendReqPush> NetworkManager::GetPendingFriendRequests() {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_friend_requests_;
}

void NetworkManager::RemovePendingRequest(uint64_t req_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_friend_requests_.erase(
        std::remove_if(pending_friend_requests_.begin(), pending_friend_requests_.end(),
                       [req_id](const im::FriendReqPush& req) { return req.req_id() == req_id; }),
        pending_friend_requests_.end());
}

bool NetworkManager::SendP2PMessage(uint64_t receiver_id, const std::string& content, std::string& error_msg) {
    constexpr int kMaxAttempts = 3;
    constexpr auto kAckTimeout = std::chrono::milliseconds(2000);

    im::P2PMessage req;
    req.set_sender_id(user_id_);
    req.set_receiver_id(receiver_id);
    req.set_content(content);
    req.set_timestamp(time(nullptr));

    im::Envelope env;
    env.set_seq(NextSeq());
    env.set_cmd(im::CMD_P2P_MSG_REQ);
    env.set_timestamp(time(nullptr));
    req.set_client_msg_id(GenerateClientMsgId());
    *env.mutable_p2p_msg_req() = req;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_p2p_messages_[env.seq()] = PendingP2PMessage{env, req, 0};
    }

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_p2p_messages_.contains(env.seq())) {
                pending_p2p_messages_[env.seq()].attempts = attempt;
            }
        }

        if (!SendEnvelope(env)) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_p2p_messages_.erase(env.seq());
            error_msg = "Network error while sending message";
            return false;
        }

        im::Envelope resp_env;
        if (!WaitForResponse(env.seq(), resp_env, im::CMD_MSG_ACK, kAckTimeout)) {
            continue;
        }

        const auto& resp = resp_env.msg_ack();
        if (resp.success()) {
            req.set_msg_id(resp.msg_id());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_p2p_messages_.erase(env.seq());
                p2p_chat_history_[receiver_id].push_back(req);
            }
            return true;
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_p2p_messages_.erase(env.seq());
            error_msg = resp.error_msg();
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_p2p_messages_.erase(env.seq());
    }
    error_msg = "Message ACK timeout after retransmission";
    return false;
}

bool NetworkManager::SyncMessages(std::string& error_msg) {
    im::SyncMessagesReq req;
    req.set_user_id(user_id_);

    im::Envelope env;
    env.set_cmd(im::CMD_SYNC_MSGS_REQ);
    env.set_timestamp(time(nullptr));
    *env.mutable_sync_msgs_req() = req;

    im::Envelope resp_env;
    if (!SendRequestAndWait(env, resp_env, im::CMD_SYNC_MSGS_RES)) {
        error_msg = "Request timeout or network error";
        return false;
    }

    const auto& resp = resp_env.sync_msgs_res();
    if (resp.success()) {
        std::lock_guard<std::mutex> lock(mutex_);
        p2p_chat_history_.clear();
        for (int i = resp.messages_size() - 1; i >= 0; --i) {
            const auto& msg = resp.messages(i);
            uint64_t chat_partner_id = (msg.sender_id() == user_id_) ? msg.receiver_id() : msg.sender_id();
            p2p_chat_history_[chat_partner_id].push_back(msg);
        }
        return true;
    } else {
        error_msg = resp.error_msg();
        return false;
    }
}

std::vector<im::P2PMessage> NetworkManager::GetP2PHistory(uint64_t receiver_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return p2p_chat_history_[receiver_id];
}
