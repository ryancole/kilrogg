#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace krg {

// What a client's link was last measured to carry. Rate control otherwise
// starts every connection at the ceiling, which means a receiver that
// reconnects — and with automatic reconnection, one that reconnects on every
// hiccup of the very link that is too slow — rediscovers that the only way it
// can: by overflowing the send queue and spending an IDR on the recovery.
//
// The memory is deliberately short. A client back within a minute has the link
// it just had; ten minutes later it may be somewhere else entirely, and a rate
// learned on a bad afternoon should not pin the picture down for the evening.
// In between, the recalled rate relaxes toward the ceiling, so a stale reading
// costs at most the difference between it and what rate control would have
// probed its way to anyway.
//
// Both calls take the time to judge against rather than reading the clock
// themselves. The sender leaves it defaulted; the point of the seam is that the
// hour-long expiry above can be exercised without an hour going by.
class RateMemory {
public:
    using Clock = std::chrono::steady_clock;

    void remember(const std::string& peer, uint32_t bps, Clock::time_point now = Clock::now()) {
        if (peer.empty()) return;
        for (Entry& e : entries_) {
            if (e.peer == peer) {
                e.bps = bps;
                e.at = now;
                return;
            }
        }
        if (entries_.size() >= kMaxEntries) entries_.erase(entries_.begin());
        entries_.push_back({peer, bps, now});
    }

    // 0 when nothing is known, which RateControl reads as "start at the
    // ceiling" — the right guess for a link nothing has been measured about.
    uint32_t recall(const std::string& peer, uint32_t ceiling_bps,
                    Clock::time_point now = Clock::now()) const {
        for (const Entry& e : entries_) {
            if (e.peer != peer) continue;
            const double age = std::chrono::duration<double>(now - e.at).count();
            if (age >= kForgetSecs) return 0;
            const uint32_t bps = std::min(e.bps, ceiling_bps);
            if (age <= kFreshSecs) return bps;
            const double t = (age - kFreshSecs) / (kForgetSecs - kFreshSecs);
            return bps + static_cast<uint32_t>((ceiling_bps - bps) * t);
        }
        return 0;
    }

private:
    static constexpr double kFreshSecs = 60.0;
    static constexpr double kForgetSecs = 600.0;
    static constexpr size_t kMaxEntries = 8;

    struct Entry {
        std::string peer;
        uint32_t bps;
        Clock::time_point at;
    };
    std::vector<Entry> entries_; // oldest first; evicted from the front
};

} // namespace krg
