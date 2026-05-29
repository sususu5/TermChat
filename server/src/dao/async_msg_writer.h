#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>
#include "../core/mpsc_queue.h"
#include "message_service.pb.h"
#include "msg_scylla_dao.h"

class AsyncMsgWriter {
public:
    static AsyncMsgWriter* GetInstance() {
        static AsyncMsgWriter instance;
        return &instance;
    };

    void Start();
    void Stop();
    using PersistCallback = std::function<void(const im::P2PMessage& msg, bool success)>;
    void Enqueue(im::P2PMessage msg, PersistCallback callback = {});

private:
    struct QueueItem {
        im::P2PMessage msg;
        PersistCallback callback;
    };

    AsyncMsgWriter() = default;
    ~AsyncMsgWriter() { Stop(); };

    void WorkerLoop();
    bool PersistBatch(const std::vector<QueueItem>& batch_buffer);
    void NotifyCallbacks(const std::vector<QueueItem>& batch_buffer, bool success);

    MPSCQueue<QueueItem> queue_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> queued_items_{0};
    MsgScyllaDao dao_;

    static const size_t kBatchSize = 100;
};
