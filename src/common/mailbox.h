#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

namespace krg {

// Single-slot, latest-wins handoff between one producer and one consumer.
// If the consumer hasn't taken the previous item yet, `merge` folds the
// pending (older) item into the incoming one before it takes the slot —
// used to union dirty rects so a dropped frame's updates aren't lost.
template <typename T>
class Mailbox {
public:
    using MergeFn = std::function<void(T& incoming, T& pending)>;

    explicit Mailbox(MergeFn merge = {}) : merge_(std::move(merge)) {}

    void push(T item) {
        {
            std::lock_guard lock(mutex_);
            if (slot_ && merge_) merge_(item, *slot_);
            slot_ = std::move(item);
        }
        cv_.notify_one();
    }

    // Blocks until an item arrives or stop() is called.
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return slot_.has_value() || stopped_; });
        return take();
    }

    template <typename Rep, typename Period>
    std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [this] { return slot_.has_value() || stopped_; });
        return take();
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    std::optional<T> take() {
        if (!slot_) return std::nullopt;
        std::optional<T> out = std::move(slot_);
        slot_.reset();
        return out;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<T> slot_;
    MergeFn merge_;
    bool stopped_ = false;
};

} // namespace krg
