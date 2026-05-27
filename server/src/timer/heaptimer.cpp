#include "heaptimer.h"

void HeapTimer::SwapNode_(size_t i, size_t j) {
    assert(i >= 0 && i < heap_.size());
    assert(j >= 0 && j < heap_.size());
    std::swap(heap_[i], heap_[j]);
    ref_[heap_[i].id] = i;
    ref_[heap_[j].id] = j;
}

void HeapTimer::SiftUp_(size_t i) {
    assert(i < heap_.size());
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!(heap_[parent] > heap_[i])) {
            break;
        }
        SwapNode_(i, parent);
        i = parent;
    }
}

bool HeapTimer::SiftDown_(size_t index, size_t n) {
    assert(index >= 0 && index < heap_.size());
    assert(n >= 0 && n <= heap_.size());
    auto i = index;
    auto child = 2 * i + 1;
    while (child < n) {
        if (child + 1 < n && heap_[child + 1] < heap_[child]) child++;
        if (heap_[i] < heap_[child]) break;
        SwapNode_(i, child);
        i = child;
        child = 2 * i + 1;
    }
    return i > index;
}

void HeapTimer::Del_(size_t index) {
    assert(index < heap_.size());
    const int deleted_id = heap_[index].id;

    if (index < heap_.size() - 1) {
        SwapNode_(index, heap_.size() - 1);
        if (!SiftDown_(index, heap_.size() - 1)) {
            SiftUp_(index);
        }
    }
    ref_[deleted_id] = -1;
    heap_.pop_back();
}

void HeapTimer::Adjust(int id, int newExpires) {
    assert(id >= 0 && static_cast<size_t>(id) < ref_.size() && ref_[id] != -1);
    heap_[ref_[id]].expires = Clock::now() + MS(newExpires);
    if (!SiftDown_(ref_[id], heap_.size())) {
        SiftUp_(ref_[id]);
    }
}

void HeapTimer::Add(int id, int timeOut) {
    assert(id >= 0);
    if (static_cast<size_t>(id) >= ref_.size()) {
        ref_.resize(static_cast<size_t>(id) * 2 + 1, -1);
    }

    if (ref_[id] != -1) {
        // If the timer already exists, update the expiration time
        size_t tmp = ref_[id];
        heap_[tmp].expires = Clock::now() + MS(timeOut);
        if (!SiftDown_(tmp, heap_.size())) {
            SiftUp_(tmp);
        }
    } else {
        // If the timer does not exist, add a new timer
        size_t n = heap_.size();
        ref_[id] = n;
        heap_.push_back({id, Clock::now() + MS(timeOut)});
        SiftUp_(n);
    }
}

void HeapTimer::Remove(int id) {
    if (id < 0 || static_cast<size_t>(id) >= ref_.size() || ref_[id] == -1) {
        return;
    }
    Del_(ref_[id]);
}

// Delete the specified timer and trigger the callback function
void HeapTimer::DoWork(int id) {
    if (id < 0 || static_cast<size_t>(id) >= ref_.size() || ref_[id] == -1) return;
    if (callback_) {
        callback_(id);
    }
    Remove(id);
}

// Clear the timer which has expired
void HeapTimer::Tick() {
    while (!heap_.empty()) {
        TimerNode node = heap_.front();
        if (std::chrono::duration_cast<MS>(node.expires - Clock::now()).count() > 0) break;
        if (callback_) {
            callback_(node.id);
        }
        Remove(node.id);
    }
}

void HeapTimer::Pop() {
    assert(heap_.size() > 0);
    Del_(0);
}

void HeapTimer::Clear() {
    ref_.assign(ref_.size(), -1);
    heap_.clear();
}

int HeapTimer::GetNextTick() {
    Tick();
    int res = -1;
    if (!heap_.empty()) {
        res = static_cast<int>(std::chrono::duration_cast<MS>(heap_.front().expires - Clock::now()).count());
        if (res < 0) res = 0;
    }
    return res;
}
