#pragma once
#include <cstddef>
#include <cstdint>

namespace krg {

// Decides what bitrate to ask the encoder for, from what the send queue is
// doing. AIMD: back off hard when the link says it cannot carry the current
// rate, creep back up only while the link is demonstrably keeping pace.
//
// The signals come from PacketSender, counted at the socket rather than at the
// encoder, so they describe what the receiver actually gets. Two of them
// matter:
//
//   backlog flush   the queue hit its ceiling and video was thrown away. The
//                   link is over capacity, and the bytes that did get out
//                   during the interval measure what it can carry — while the
//                   queue is backed up the send thread never idles, so that
//                   figure is the link, not the encoder.
//   queue depth     no overflow yet, but frames are sitting in the queue
//                   instead of on the wire. The same problem, caught earlier
//                   and paid for in a smaller cut.
//
// Both readings have to be checked against what the link actually carried,
// because a queue can also fill without the link being at fault: the encoder
// hands over a frame in one burst, and if the send thread has not been
// scheduled yet the queue is briefly deep through no fault of the network. On
// loopback that shows up as megabyte peaks with a standing depth of zero. So
// depth is sampled as it stands rather than as it peaked, and no reading cuts
// the rate while the wire is carrying everything it was asked for.
//
// Without this the failure mode is self-reinforcing: an overflow forces an IDR
// (every queued P-frame after a dropped one is undecodable), the IDR is the
// largest packet the encoder emits, and pushing it into a link that just
// proved it cannot keep up overflows the queue again.
class RateControl {
public:
    // One interval's worth of send-queue accounting.
    struct Sample {
        uint64_t bytes_sent = 0;      // reached the socket during the interval
        uint64_t backlog_flushes = 0; // overflows during the interval
        size_t queued_bytes = 0;      // depth as it stood at the end of the interval
        size_t queue_capacity = 0;    // the backlog budget that depth is measured against
        double secs = 0;              // the interval itself
    };

    struct Decision {
        uint32_t bps = 0;       // 0 = no change; leave the encoder alone
        bool congested = false; // the change was a cut, not a probe upward
    };

    // `ceiling_bps` is --bitrate: the rate to use when the link allows it and
    // the most that will ever be asked for. `floor_bps` bounds how ugly the
    // picture is allowed to get before the answer is "this link cannot do it".
    // `start_bps` is where to begin, clamped into [floor, ceiling]; 0 means the
    // ceiling, which is the right guess for a link nothing is known about. A
    // client that was here a moment ago passes what it had converged on, so a
    // reconnect over a link that is still slow does not have to rediscover it
    // by overflowing the queue again.
    RateControl(uint32_t ceiling_bps, uint32_t floor_bps, uint32_t start_bps = 0);

    // Folds one interval in and returns the target to switch to, if any.
    Decision update(const Sample& s);

    uint32_t target_bps() const { return target_bps_; }

private:
    uint32_t ceiling_bps_;
    uint32_t floor_bps_;
    uint32_t step_bps_;   // additive increase, per probe
    uint32_t target_bps_; // what the encoder was last told to do

    double secs_since_cut_; // starts ready to cut; see the constructor
    int clean_intervals_ = 0;
    int deep_intervals_ = 0;
};

} // namespace krg
