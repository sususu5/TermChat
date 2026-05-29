#include "async_msg_writer.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include "../log/log.h"

namespace {
bool MetricsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TERMCHAT_METRICS");
        if (!value) return false;
        std::string_view flag(value);
        return flag == "1" || flag == "true" || flag == "TRUE" || flag == "on" || flag == "ON";
    }();
    return enabled;
}
}  // namespace

void AsyncMsgWriter::Start() {
    bool expected = false;
    if (running_.compare_exchange_strong(expected, true)) {
        worker_ = std::thread(&AsyncMsgWriter::WorkerLoop, this);
        LOG_INFO("AsyncMsgWriter started.");
    }
}

void AsyncMsgWriter::Stop() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false)) {
        if (worker_.joinable()) {
            worker_.join();
        }
        LOG_INFO("AsyncMsgWriter stopped.");
    }
}

void AsyncMsgWriter::Enqueue(im::P2PMessage msg, PersistCallback callback) {
    queued_items_.fetch_add(1, std::memory_order_relaxed);
    queue_.enqueue(QueueItem{std::move(msg), std::move(callback)});
}

bool AsyncMsgWriter::PersistBatch(const std::vector<QueueItem>& batch_buffer) {
    std::vector<im::P2PMessage> messages;
    messages.reserve(batch_buffer.size());
    for (const auto& item : batch_buffer) {
        messages.push_back(item.msg);
    }
    return dao_.InsertBatch(messages);
}

void AsyncMsgWriter::NotifyCallbacks(const std::vector<QueueItem>& batch_buffer, bool success) {
    for (const auto& item : batch_buffer) {
        if (item.callback) {
            item.callback(item.msg, success);
        }
    }
}

void AsyncMsgWriter::WorkerLoop() {
    std::vector<QueueItem> batch_buffer;
    batch_buffer.reserve(kBatchSize);

    const int kMaxRetries = 3;
    const int kBaseWaitMs = 50;
    const int kMaxWaitMs = 1000;
    uint64_t batches_since_report = 0;
    uint64_t messages_since_report = 0;
    uint64_t persist_us_since_report = 0;
    uint64_t failed_batches_since_report = 0;
    const bool metrics_enabled = MetricsEnabled();

    LOG_INFO("AsyncMsgWriter WorkerLoop started.");

    while (running_) {
        auto count = queue_.dequeue_bulk(std::back_inserter(batch_buffer), kBatchSize);

        if (count > 0) {
            queued_items_.fetch_sub(count, std::memory_order_relaxed);
            bool success = false;
            int retry_count = 0;
            auto persist_start =
                metrics_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            while (retry_count <= kMaxRetries) {
                if (PersistBatch(batch_buffer)) {
                    success = true;
                    break;
                }
                retry_count++;
                if (retry_count > kMaxRetries) break;
                auto wait_ms = kBaseWaitMs * (1 << (retry_count - 1));
                if (wait_ms > kMaxWaitMs) wait_ms = kMaxWaitMs;
                LOG_WARN("Batch insert failed, retrying {}/{} in {}ms...", retry_count, kMaxRetries, wait_ms);
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
            }
            auto persist_us = metrics_enabled ? std::chrono::duration_cast<std::chrono::microseconds>(
                                                    std::chrono::steady_clock::now() - persist_start)
                                                    .count()
                                              : 0;

            if (!success) {
                LOG_ERROR("Failed to insert batch of {} messages.", count);
                failed_batches_since_report++;
            }
            batches_since_report++;
            messages_since_report += count;
            persist_us_since_report += static_cast<uint64_t>(persist_us);
            if (metrics_enabled && (batches_since_report >= 100 || persist_us >= 100000)) {
                std::fprintf(stderr,
                             "[metrics] async_writer batches=%lu messages=%lu avg_persist_us=%lu last_persist_us=%ld "
                             "failed_batches=%lu queued=%zu\n",
                             batches_since_report, messages_since_report,
                             persist_us_since_report / batches_since_report, persist_us, failed_batches_since_report,
                             queued_items_.load(std::memory_order_relaxed));
                batches_since_report = 0;
                messages_since_report = 0;
                persist_us_since_report = 0;
                failed_batches_since_report = 0;
            }
            NotifyCallbacks(batch_buffer, success);
            batch_buffer.clear();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    while (!queue_.empty()) {
        auto count = queue_.dequeue_bulk(std::back_inserter(batch_buffer), kBatchSize);
        if (count > 0) {
            queued_items_.fetch_sub(count, std::memory_order_relaxed);
            const bool success = PersistBatch(batch_buffer);
            if (!success) {
                LOG_ERROR("Failed to insert batch of {} messages.", count);
            }
            NotifyCallbacks(batch_buffer, success);
            batch_buffer.clear();
        }
    }

    LOG_INFO("AsyncMsgWriter WorkerLoop stopped. Flushed remaining messages.");
}
