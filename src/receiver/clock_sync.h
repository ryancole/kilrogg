#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

#include "common/protocol.h"

namespace krg {

// The two machines' clocks share no epoch, so a sender timestamp means nothing
// locally until the offset between them is known. Ping/pong gives it: at the
// midpoint of a round trip both clocks are, by assumption, reading the same
// instant. The estimate keeps the sample with the shortest round trip, since
// that is the one least polluted by queueing in either direction.
//
// "Shortest" has to mean shortest lately, though. Two machines' clocks drift
// apart over a session, which is what the later probes are for; keeping the
// best sample forever means the first lucky one wins and every probe after it
// is discarded, leaving the offset to rot. So the best reading expires, and an
// expired one is replaced by whatever the next probe says regardless of how it
// compares — a slightly queued sample now beats a pristine one from an hour ago.
class ClockSync {
public:
    void on_pong(const Pong& pong, int64_t now) {
        int64_t rtt = now - pong.client_us;
        if (rtt < 0) return;
        if (rtt >= best_rtt_us_ && now - best_at_us_ < kBestRttWindowUs) return;
        best_rtt_us_ = rtt;
        best_at_us_ = now;
        rtt_us_.store(rtt, std::memory_order_relaxed);
        offset_us_.store(pong.server_us - (pong.client_us + rtt / 2), std::memory_order_relaxed);
        valid_.store(true, std::memory_order_relaxed);
    }

    bool valid() const { return valid_.load(std::memory_order_relaxed); }
    // Sender clock reading, in receiver-clock terms.
    int64_t to_local(int64_t sender_us) const {
        return sender_us - offset_us_.load(std::memory_order_relaxed);
    }
    double rtt_ms() const { return rtt_us_.load(std::memory_order_relaxed) / 1000.0; }

private:
    // Long enough to hold a good handful of the 2-second probes, so the
    // estimate still settles on a quiet round trip rather than a queued one;
    // short enough that drift is corrected while it is still small.
    static constexpr int64_t kBestRttWindowUs = 30'000'000;

    // Receive thread only.
    int64_t best_rtt_us_ = std::numeric_limits<int64_t>::max();
    int64_t best_at_us_ = 0;
    std::atomic<int64_t> offset_us_{0};
    std::atomic<int64_t> rtt_us_{0};
    std::atomic<bool> valid_{false};
};

} // namespace krg
