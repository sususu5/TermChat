#pragma once

#include <arpa/inet.h>
#include <assert.h>
#include <time.h>
#include <chrono>
#include <functional>

using TimeoutCallBack = std::function<void(int)>;
using Clock = std::chrono::high_resolution_clock;
using MS = std::chrono::milliseconds;
using TimeStamp = Clock::time_point;

struct TimerNode {
    int id;
    TimeStamp expires;
    bool operator<(const TimerNode& t) { return expires < t.expires; }
    bool operator>(const TimerNode& t) { return expires > t.expires; }
};

class HeapTimer {
public:
    HeapTimer() {
        heap_.reserve(64);
        ref_.assign(65536, -1);
    }
    ~HeapTimer() { Clear(); }

    void Adjust(int id, int newExpires);
    void Add(int id, int timeOut);
    void Remove(int id);
    void DoWork(int id);
    void Clear();
    void Tick();
    void Pop();
    int GetNextTick();
    void SetCallBack(TimeoutCallBack cb) { callback_ = cb; }

private:
    void Del_(size_t index);
    void SiftUp_(size_t index);
    bool SiftDown_(size_t index, size_t n);
    void SwapNode_(size_t index, size_t j);

    std::vector<TimerNode> heap_;
    // id -> index of the heap_
    std::vector<size_t> ref_;
    TimeoutCallBack callback_;
};
