#pragma once

#include <cstdint>
#include "../service/auth_service.h"
#include "../service/friend_service.h"
#include "../service/msg_service.h"
#include "core/tcp_connection.h"
#include "protocol.pb.h"

/**
 * ProtobufHandler - Handles binary protobuf protocol over TCP
 *
 * Wire format: [4-byte length (big-endian)] [protobuf payload]
 *
 * The handler maintains a simple state machine:
 * 1. Read length prefix (4 bytes)
 * 2. Read payload (length bytes)
 * 3. Parse and dispatch based on CommandType
 * 4. Send response with same wire format
 */
class ProtobufHandler : public ProtocolHandler {
public:
    ProtobufHandler(TcpConnection* conn, AuthService* auth_service, FriendService* friend_service,
                    MsgService* msg_service, ThreadPool* thread_pool);
    ~ProtobufHandler() override = default;

    bool Process(Buffer& read_buff, Buffer& write_buff) override;
    bool IsKeepAlive() const override { return true; }

private:
    // Message codec
    bool TryDecodeMessage(Buffer& read_buff, im::Envelope& envelope);
    void EncodeMessage(const im::Envelope& envelope, Buffer& write_buff);

    // Command dispatcher
    void Dispatch(const im::Envelope& request, im::Envelope& response);

    // Auth command handlers
    void HandleRegister(const im::Envelope& request, im::Envelope& response);
    void HandleLogin(const im::Envelope& request, im::Envelope& response);

    // Friend command handlers (require authentication)
    void HandleAddFriend(const im::Envelope& request, im::Envelope& response);
    void HandleHandleFriend(const im::Envelope& request, im::Envelope& response);
    void HandleGetFriendList(const im::Envelope& request, im::Envelope& response);

    // Message command handlers
    void HandleP2PMsg(const im::Envelope& request, im::Envelope& response);
    void HandleMessageAck(const im::Envelope& request, im::Envelope& response);
    void HandleMessageAckBatch(const im::Envelope& request, im::Envelope& response);
    void HandleSyncMessages(const im::Envelope& request, im::Envelope& response);

    void HandleUnknown(const im::Envelope& request, im::Envelope& response);

    // Helper to check authentication and get user_id
    bool RequireAuth(im::Envelope& response, im::CommandType resp_cmd);
    uint64_t CurrentUserId() const;

    // Connection (for session state)
    TcpConnection* conn_;

    // Services (injected dependencies)
    AuthService* auth_service_;
    FriendService* friend_service_;
    MsgService* msg_service_;
    ThreadPool* thread_pool_;

    // Constants
    static constexpr size_t kHeaderSize = 4;            // Length prefix size
    static constexpr size_t kMaxMessageSize = 1 << 20;  // 1MB max message size
};

// Specialization for std::format to handle im::CommandType directly
template <>
struct std::formatter<im::CommandType> : std::formatter<int> {
    auto Format(im::CommandType cmd, format_context& ctx) const {
        return formatter<int>::format(static_cast<int>(cmd), ctx);
    }
};
