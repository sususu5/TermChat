#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "protocol.pb.h"

class NetworkManager {
public:
    using OnErrorCallback = std::function<void(const std::string& error_msg)>;
    using OnFriendRequestCallback = std::function<void(const im::FriendReqPush& req)>;
    using OnFriendStatusCallback = std::function<void(const im::FriendStatusPush& status)>;
    using OnMessageCallback = std::function<void(const im::P2PMessage& msg)>;

    static NetworkManager& GetInstance() {
        static NetworkManager instance;
        return instance;
    }

    bool Connect(const std::string& host, int port);
    void SetOnErrorCallback(OnErrorCallback callback) { on_error_callback_ = callback; }
    void SetOnFriendRequestCallback(OnFriendRequestCallback callback) { on_friend_request_callback_ = callback; }
    void SetOnFriendStatusCallback(OnFriendStatusCallback callback) { on_friend_status_callback_ = callback; }
    void SetOnMessageCallback(OnMessageCallback callback) { on_message_callback_ = callback; }

    // Auth Service
    bool Register(const std::string& username, const std::string& password, std::string& error_msg);
    bool Login(const std::string& username, const std::string& password, std::string& error_msg);
    bool Logout(std::string& error_msg);

    // Friend Service
    bool AddFriend(uint64_t receiver_id, const std::string& verify_msg, std::string& error_msg);
    bool HandleFriendRequest(uint64_t req_id, uint64_t sender_id, im::FriendAction action, std::string& error_msg);
    bool GetFriendList(std::vector<im::User>& friend_info_list, std::string& error_msg);

    // Message Service
    bool SendP2PMessage(uint64_t receiver_id, const std::string& content, std::string& error_msg);
    bool SyncMessages(std::string& error_msg);
    std::vector<im::P2PMessage> GetP2PHistory(uint64_t receiver_id);

    // Getters
    bool IsLoggedIn() const { return !token_.empty(); }
    const std::string& GetToken() const { return token_; }
    uint64_t GetUserId() const { return user_id_; }
    const std::string& GetUsername() const { return username_; }

    std::vector<im::FriendReqPush> GetPendingFriendRequests();
    void RemovePendingRequest(uint64_t req_id);

private:
    NetworkManager() = default;
    ~NetworkManager() { Disconnect(); }

    // Internal Helpers
    bool SendEnvelope(const im::Envelope& env);
    bool SendRequestAndWait(im::Envelope request, im::Envelope& response, im::CommandType expected_cmd);
    bool WaitForResponse(uint64_t seq, im::Envelope& response, im::CommandType expected_cmd,
                         std::chrono::milliseconds timeout);
    uint64_t NextSeq();
    uint64_t GenerateClientMsgId();
    void ListenerLoop();
    void HeartbeatLoop();
    void ClearAuth();
    void Disconnect();

    void ReportError(const std::string& msg) {
        if (on_error_callback_) {
            on_error_callback_(msg);
        }
    }

    // Network State
    int sock_ = -1;
    std::atomic<bool> connected_{false};
    std::string token_;
    uint64_t user_id_ = 0;
    std::string username_;

    // Async Handling
    std::thread listener_thread_;
    std::thread heartbeat_thread_;
    std::atomic<bool> running_{false};

    std::mutex mutex_;
    std::condition_variable cv_response_;
    uint64_t next_seq_ = 1;
    std::unordered_map<uint64_t, im::Envelope> response_by_seq_;

    struct PendingP2PMessage {
        im::Envelope envelope;
        im::P2PMessage message;
        int attempts = 0;
    };

    std::unordered_map<uint64_t, PendingP2PMessage> pending_p2p_messages_;

    // Callbacks & Storage
    OnErrorCallback on_error_callback_;
    OnFriendRequestCallback on_friend_request_callback_;
    OnFriendStatusCallback on_friend_status_callback_;
    OnMessageCallback on_message_callback_;

    std::vector<im::FriendReqPush> pending_friend_requests_;
    std::unordered_map<uint64_t, std::vector<im::P2PMessage>> p2p_chat_history_;
};
