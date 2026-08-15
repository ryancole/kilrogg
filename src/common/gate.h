#pragma once
#include <condition_variable>
#include <mutex>

namespace krg {

// A latch a worker thread parks on until someone opens it. Used to stop the
// sender's capture thread while no client is attached: acquiring and copying a
// full desktop image every frame costs the same GPU work whether or not anyone
// is watching it.
class Gate {
public:
    void set(bool open) {
        {
            std::lock_guard lock(mutex_);
            if (open_ == open) return;
            open_ = open;
        }
        if (open) cv_.notify_all();
    }

    bool is_set() const {
        std::lock_guard lock(mutex_);
        return open_;
    }

    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

} // namespace krg
