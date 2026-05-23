#include "msg_scylla_dao.h"
#include <cassandra.h>
#include <algorithm>
#include <chrono>
#include "../log/log.h"
#include "../pool/scylla_session.h"
#include "../utils/id_generator.h"

namespace {
std::string CassFutureError(CassFuture* future) {
    const char* message = nullptr;
    size_t message_length = 0;
    cass_future_error_message(future, &message, &message_length);
    if (message == nullptr || message_length == 0) {
        return "unknown error";
    }
    return std::string(message, message_length);
}
}  // namespace

namespace {
int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

bool MsgScyllaDao::InsertMessage(const im::P2PMessage& msg) {
    auto* session = ScyllaSession::Instance()->Session();
    if (!session) {
        LOG_ERROR("Scylla session is not initialized");
        return false;
    }

    // Use a batch to ensure atomicity and reduce round-trips
    CassBatch* batch = cass_batch_new(CASS_BATCH_TYPE_LOGGED);

    // 1. Insert into conversation history
    constexpr const char* k_insert_history =
        "INSERT INTO im.messages (conversation_id, message_id, timestamp, sender_id, "
        "receiver_id, content_type, content) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";

    CassStatement* stmt_history = cass_statement_new(k_insert_history, 7);
    auto p2p_conv_id = IdGenerator::GenerateP2PConvId(msg.sender_id(), msg.receiver_id());
    cass_statement_bind_string(stmt_history, 0, p2p_conv_id.c_str());
    cass_statement_bind_int64(stmt_history, 1, static_cast<cass_int64_t>(msg.msg_id()));
    cass_statement_bind_int64(stmt_history, 2, static_cast<cass_int64_t>(msg.timestamp()));
    cass_statement_bind_int64(stmt_history, 3, static_cast<cass_int64_t>(msg.sender_id()));
    cass_statement_bind_int64(stmt_history, 4, static_cast<cass_int64_t>(msg.receiver_id()));
    cass_statement_bind_int32(stmt_history, 5, static_cast<cass_int32_t>(msg.content_type()));
    const std::string& content = msg.content();
    cass_statement_bind_bytes(stmt_history, 6, reinterpret_cast<const cass_byte_t*>(content.data()), content.size());

    cass_batch_add_statement(batch, stmt_history);
    cass_statement_free(stmt_history);

    // 2. Insert into inbox table for incremental sync.
    constexpr const char* k_insert_inbox_by_id =
        "INSERT INTO im.user_messages_by_id (user_id, message_id, sender_id, receiver_id, content_type, content, "
        "timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";

    CassStatement* stmt_inbox_by_id_receiver = cass_statement_new(k_insert_inbox_by_id, 7);
    cass_statement_bind_int64(stmt_inbox_by_id_receiver, 0, static_cast<cass_int64_t>(msg.receiver_id()));
    cass_statement_bind_int64(stmt_inbox_by_id_receiver, 1, static_cast<cass_int64_t>(msg.msg_id()));
    cass_statement_bind_int64(stmt_inbox_by_id_receiver, 2, static_cast<cass_int64_t>(msg.sender_id()));
    cass_statement_bind_int64(stmt_inbox_by_id_receiver, 3, static_cast<cass_int64_t>(msg.receiver_id()));
    cass_statement_bind_int32(stmt_inbox_by_id_receiver, 4, static_cast<cass_int32_t>(msg.content_type()));
    cass_statement_bind_bytes(stmt_inbox_by_id_receiver, 5, reinterpret_cast<const cass_byte_t*>(content.data()),
                              content.size());
    cass_statement_bind_int64(stmt_inbox_by_id_receiver, 6, static_cast<cass_int64_t>(msg.timestamp()));
    cass_batch_add_statement(batch, stmt_inbox_by_id_receiver);
    cass_statement_free(stmt_inbox_by_id_receiver);

    CassStatement* stmt_inbox_by_id_sender = cass_statement_new(k_insert_inbox_by_id, 7);
    cass_statement_bind_int64(stmt_inbox_by_id_sender, 0, static_cast<cass_int64_t>(msg.sender_id()));
    cass_statement_bind_int64(stmt_inbox_by_id_sender, 1, static_cast<cass_int64_t>(msg.msg_id()));
    cass_statement_bind_int64(stmt_inbox_by_id_sender, 2, static_cast<cass_int64_t>(msg.sender_id()));
    cass_statement_bind_int64(stmt_inbox_by_id_sender, 3, static_cast<cass_int64_t>(msg.receiver_id()));
    cass_statement_bind_int32(stmt_inbox_by_id_sender, 4, static_cast<cass_int32_t>(msg.content_type()));
    cass_statement_bind_bytes(stmt_inbox_by_id_sender, 5, reinterpret_cast<const cass_byte_t*>(content.data()),
                              content.size());
    cass_statement_bind_int64(stmt_inbox_by_id_sender, 6, static_cast<cass_int64_t>(msg.timestamp()));
    cass_batch_add_statement(batch, stmt_inbox_by_id_sender);
    cass_statement_free(stmt_inbox_by_id_sender);

    // Execute Batch·
    CassFuture* future = cass_session_execute_batch(session, batch);
    cass_future_wait(future);

    bool ok = true;
    if (cass_future_error_code(future) != CASS_OK) {
        LOG_ERROR("Scylla batch insert failed: {}", CassFutureError(future));
        ok = false;
    }

    cass_future_free(future);
    cass_batch_free(batch);
    return ok;
}

bool MsgScyllaDao::InsertBatch(const std::vector<im::P2PMessage>& msgs) {
    if (msgs.empty()) return true;

    auto* session = ScyllaSession::Instance()->Session();
    if (!session) {
        LOG_ERROR("Scylla session is not initialized");
        return false;
    }

    CassBatch* batch = cass_batch_new(CASS_BATCH_TYPE_LOGGED);

    constexpr const char* k_insert_history =
        "INSERT INTO im.messages (conversation_id, message_id, timestamp, sender_id, "
        "receiver_id, content_type, content) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";

    constexpr const char* k_insert_inbox_by_id =
        "INSERT INTO im.user_messages_by_id (user_id, message_id, sender_id, receiver_id, content_type, content, "
        "timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";

    for (const auto& msg : msgs) {
        // 1. Conversation history
        CassStatement* stmt1 = cass_statement_new(k_insert_history, 7);
        auto p2p_conv_id = IdGenerator::GenerateP2PConvId(msg.sender_id(), msg.receiver_id());
        cass_statement_bind_string(stmt1, 0, p2p_conv_id.c_str());
        cass_statement_bind_int64(stmt1, 1, static_cast<cass_int64_t>(msg.msg_id()));
        cass_statement_bind_int64(stmt1, 2, static_cast<cass_int64_t>(msg.timestamp()));
        cass_statement_bind_int64(stmt1, 3, static_cast<cass_int64_t>(msg.sender_id()));
        cass_statement_bind_int64(stmt1, 4, static_cast<cass_int64_t>(msg.receiver_id()));
        cass_statement_bind_int32(stmt1, 5, static_cast<cass_int32_t>(msg.content_type()));
        const std::string& content = msg.content();
        cass_statement_bind_bytes(stmt1, 6, reinterpret_cast<const cass_byte_t*>(content.data()), content.size());
        cass_batch_add_statement(batch, stmt1);
        cass_statement_free(stmt1);

        // 2. Receiver inbox
        CassStatement* stmt2_by_id = cass_statement_new(k_insert_inbox_by_id, 7);
        cass_statement_bind_int64(stmt2_by_id, 0, static_cast<cass_int64_t>(msg.receiver_id()));
        cass_statement_bind_int64(stmt2_by_id, 1, static_cast<cass_int64_t>(msg.msg_id()));
        cass_statement_bind_int64(stmt2_by_id, 2, static_cast<cass_int64_t>(msg.sender_id()));
        cass_statement_bind_int64(stmt2_by_id, 3, static_cast<cass_int64_t>(msg.receiver_id()));
        cass_statement_bind_int32(stmt2_by_id, 4, static_cast<cass_int32_t>(msg.content_type()));
        cass_statement_bind_bytes(stmt2_by_id, 5, reinterpret_cast<const cass_byte_t*>(content.data()), content.size());
        cass_statement_bind_int64(stmt2_by_id, 6, static_cast<cass_int64_t>(msg.timestamp()));
        cass_batch_add_statement(batch, stmt2_by_id);
        cass_statement_free(stmt2_by_id);

        // 3. Sender inbox
        CassStatement* stmt3_by_id = cass_statement_new(k_insert_inbox_by_id, 7);
        cass_statement_bind_int64(stmt3_by_id, 0, static_cast<cass_int64_t>(msg.sender_id()));
        cass_statement_bind_int64(stmt3_by_id, 1, static_cast<cass_int64_t>(msg.msg_id()));
        cass_statement_bind_int64(stmt3_by_id, 2, static_cast<cass_int64_t>(msg.sender_id()));
        cass_statement_bind_int64(stmt3_by_id, 3, static_cast<cass_int64_t>(msg.receiver_id()));
        cass_statement_bind_int32(stmt3_by_id, 4, static_cast<cass_int32_t>(msg.content_type()));
        cass_statement_bind_bytes(stmt3_by_id, 5, reinterpret_cast<const cass_byte_t*>(content.data()), content.size());
        cass_statement_bind_int64(stmt3_by_id, 6, static_cast<cass_int64_t>(msg.timestamp()));
        cass_batch_add_statement(batch, stmt3_by_id);
        cass_statement_free(stmt3_by_id);
    }

    CassFuture* future = cass_session_execute_batch(session, batch);
    cass_future_wait(future);

    bool ok = true;
    if (cass_future_error_code(future) != CASS_OK) {
        LOG_ERROR("Scylla batch insert failed: {}", CassFutureError(future));
        ok = false;
    }

    cass_future_free(future);
    cass_batch_free(batch);
    return ok;
}

MsgScyllaDao::MessagePage MsgScyllaDao::GetMessagesForUserAfter(uint64_t user_id, uint64_t last_ack_msg_id,
                                                                uint32_t limit) {
    MessagePage page;
    auto* session = ScyllaSession::Instance()->Session();
    if (!session) return page;

    const uint32_t page_size = std::clamp<uint32_t>(limit == 0 ? 100 : limit, 1, 500);
    const uint32_t query_limit = page_size + 1;

    constexpr const char* k_select_after =
        "SELECT message_id, sender_id, receiver_id, content_type, content, timestamp "
        "FROM im.user_messages_by_id "
        "WHERE user_id = ? AND message_id > ? "
        "LIMIT ?;";

    CassStatement* statement = cass_statement_new(k_select_after, 3);
    cass_statement_bind_int64(statement, 0, static_cast<cass_int64_t>(user_id));
    cass_statement_bind_int64(statement, 1, static_cast<cass_int64_t>(last_ack_msg_id));
    cass_statement_bind_int32(statement, 2, static_cast<cass_int32_t>(query_limit));

    CassFuture* future = cass_session_execute(session, statement);
    cass_future_wait(future);

    if (cass_future_error_code(future) == CASS_OK) {
        const CassResult* cass_result = cass_future_get_result(future);
        CassIterator* iterator = cass_iterator_from_result(cass_result);

        while (cass_iterator_next(iterator)) {
            const CassRow* row = cass_iterator_get_row(iterator);
            im::P2PMessage msg;
            cass_int64_t msg_id, sender_id, receiver_id, ts;
            cass_int32_t c_type;
            const cass_byte_t* c_data;
            size_t c_len;

            cass_value_get_int64(cass_row_get_column(row, 0), &msg_id);
            cass_value_get_int64(cass_row_get_column(row, 1), &sender_id);
            cass_value_get_int64(cass_row_get_column(row, 2), &receiver_id);
            cass_value_get_int32(cass_row_get_column(row, 3), &c_type);
            cass_value_get_bytes(cass_row_get_column(row, 4), &c_data, &c_len);
            cass_value_get_int64(cass_row_get_column(row, 5), &ts);

            msg.set_msg_id(msg_id);
            msg.set_sender_id(sender_id);
            msg.set_receiver_id(receiver_id);
            msg.set_content_type(static_cast<im::ContentType>(c_type));
            msg.set_content(reinterpret_cast<const char*>(c_data), c_len);
            msg.set_timestamp(ts);

            page.messages.push_back(std::move(msg));
        }
        cass_iterator_free(iterator);
        cass_result_free(cass_result);
    } else {
        LOG_ERROR("Scylla query failed: {}", CassFutureError(future));
    }

    cass_future_free(future);
    cass_statement_free(statement);

    if (page.messages.size() > page_size) {
        page.has_more = true;
        page.messages.pop_back();
    }
    if (!page.messages.empty()) {
        page.next_ack_msg_id = page.messages.back().msg_id();
    } else {
        page.next_ack_msg_id = last_ack_msg_id;
    }
    return page;
}

MsgScyllaDao::ClientMsgDedupEntry MsgScyllaDao::GetClientMsgDedup(uint64_t sender_id, uint64_t client_msg_id) {
    ClientMsgDedupEntry entry;
    auto* session = ScyllaSession::Instance()->Session();
    if (!session || sender_id == 0 || client_msg_id == 0) return entry;

    constexpr const char* k_select = "SELECT server_msg_id, receiver_id, status "
                                     "FROM im.client_msg_dedup "
                                     "WHERE sender_id = ? AND client_msg_id = ?;";

    CassStatement* statement = cass_statement_new(k_select, 2);
    cass_statement_bind_int64(statement, 0, static_cast<cass_int64_t>(sender_id));
    cass_statement_bind_int64(statement, 1, static_cast<cass_int64_t>(client_msg_id));

    CassFuture* future = cass_session_execute(session, statement);
    cass_future_wait(future);

    if (cass_future_error_code(future) == CASS_OK) {
        const CassResult* cass_result = cass_future_get_result(future);
        CassIterator* iterator = cass_iterator_from_result(cass_result);
        if (cass_iterator_next(iterator)) {
            const CassRow* row = cass_iterator_get_row(iterator);
            cass_int64_t server_msg_id = 0;
            cass_int64_t receiver_id = 0;
            cass_int32_t status = 0;
            cass_value_get_int64(cass_row_get_column(row, 0), &server_msg_id);
            cass_value_get_int64(cass_row_get_column(row, 1), &receiver_id);
            cass_value_get_int32(cass_row_get_column(row, 2), &status);

            entry.found = true;
            entry.server_msg_id = static_cast<uint64_t>(server_msg_id);
            entry.receiver_id = static_cast<uint64_t>(receiver_id);
            entry.status = static_cast<im::MessageAckStatus>(status);
        }
        cass_iterator_free(iterator);
        cass_result_free(cass_result);
    } else {
        LOG_ERROR("Scylla client_msg_dedup query failed: {}", CassFutureError(future));
    }

    cass_future_free(future);
    cass_statement_free(statement);
    return entry;
}

bool MsgScyllaDao::UpsertClientMsgDedup(uint64_t sender_id, uint64_t client_msg_id, uint64_t server_msg_id,
                                        uint64_t receiver_id, im::MessageAckStatus status) {
    auto* session = ScyllaSession::Instance()->Session();
    if (!session || sender_id == 0 || client_msg_id == 0 || server_msg_id == 0) return false;

    constexpr const char* k_upsert =
        "INSERT INTO im.client_msg_dedup (sender_id, client_msg_id, server_msg_id, receiver_id, status, created_at, "
        "updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";

    const auto now_ms = NowMillis();
    CassStatement* statement = cass_statement_new(k_upsert, 7);
    cass_statement_bind_int64(statement, 0, static_cast<cass_int64_t>(sender_id));
    cass_statement_bind_int64(statement, 1, static_cast<cass_int64_t>(client_msg_id));
    cass_statement_bind_int64(statement, 2, static_cast<cass_int64_t>(server_msg_id));
    cass_statement_bind_int64(statement, 3, static_cast<cass_int64_t>(receiver_id));
    cass_statement_bind_int32(statement, 4, static_cast<cass_int32_t>(status));
    cass_statement_bind_int64(statement, 5, static_cast<cass_int64_t>(now_ms));
    cass_statement_bind_int64(statement, 6, static_cast<cass_int64_t>(now_ms));

    CassFuture* future = cass_session_execute(session, statement);
    cass_future_wait(future);

    bool ok = true;
    if (cass_future_error_code(future) != CASS_OK) {
        LOG_ERROR("Scylla client_msg_dedup upsert failed: {}", CassFutureError(future));
        ok = false;
    }

    cass_future_free(future);
    cass_statement_free(statement);
    return ok;
}
