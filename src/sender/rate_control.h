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

// Turns the rate RateControl wants on the wire into the number the encoder has
// to be given to produce it.
//
// A CBR encoder divides its bitrate by the frame rate it was configured with to
// get a budget per frame, and spends that much on each frame it is handed. The
// configured rate is the display's, because that is the fastest frames can
// arrive — but content only reaches it while something is redrawing the screen
// every single refresh. A 72 fps game on a 175 Hz panel is handed 41% of the
// frames the encoder budgeted for and therefore spends 41% of the bitrate:
// measured here, 22 Mbit/s of a commanded 40. The missing bits are not saved,
// they are simply never spent, and the picture is worse for it.
//
// RateControl cannot recover this. It only ever cuts, and it probes upward
// toward a ceiling the encoder is already dividing. So the correction goes
// here: measure the rate frames are actually being submitted at, and scale the
// commanded rate by how far short of the configured rate that falls. The wire
// rate lands on the target; the encoder just spends the same bits on fewer,
// better frames.
//
// The asymmetry is deliberate and is the same one RateControl uses. Climbing is
// gradual because the commanded rate is a bet on the content rate holding, and
// a bet that comes in high is paid for in a queue overflow, which costs an IDR.
// Falling is immediate: content speeding back up is the case where a stale
// multiplier does damage, and one interval of it is enough.
class FrameBudget {
public:
    // Ceiling on the correction. The cost of being wrong is one interval spent
    // encoding at the stale multiplier before the next sample pulls it down, so
    // this bounds that burst against the send queue's quarter-second budget.
    // It still covers the case this exists for: anything at a third of the
    // panel's rate or better is corrected in full.
    //
    // Public because it also bounds what the encoder can be asked for, and an
    // encoder has to be built for the most it will ever be asked for — a rate
    // change past the rate it was built with is clamped away without a word.
    static constexpr double kMaxMultiplier = 3.0;

    // `configured_fps` is what the encoder was told, i.e. what it divides by.
    explicit FrameBudget(uint32_t configured_fps) : configured_fps_(configured_fps) {}

    // Call once per control interval with the frames actually handed to the
    // encoder during it. Returns the rate to command, which is `target_bps`
    // scaled up by the shortfall — and is deliberately unchanged from the last
    // answer unless it moved enough to be worth another call into the MFT.
    uint32_t command(uint32_t target_bps, uint32_t frames, double secs);

    // What the commanded rate is currently being multiplied by, for the stats
    // line. 1.0 means content is keeping up with the display and nothing is
    // being corrected.
    double multiplier() const { return multiplier_; }

private:
    // Per-interval climb. At a 200 ms interval this is a couple of seconds from
    // no correction to the ceiling — slow enough that content settling at a new
    // rate is followed rather than chased.
    static constexpr double kClimbPerInterval = 0.25;
    // Above this share of the configured rate, content is keeping up and
    // nothing is corrected. The gap below 1.0 is for jitter rather than
    // generosity: a submission slot missed here and there costs a few percent
    // and means nothing, while a content rate genuinely worth correcting is a
    // long way further down.
    static constexpr double kKeepingUpShare = 0.9;
    // Below this share of the configured rate, the screen is not slow content
    // but a still one, and there is nothing there to spend a bigger budget on.
    // Re-measuring from a handful of frames would also be mostly noise.
    static constexpr double kMinContentShare = 0.1;
    // Don't re-command the MFT for less than this much of a change. A control
    // interval holds a countable number of whole frames — fourteen or fifteen
    // at 72 fps — so the measured share quantizes by some 7% at steady state,
    // and a tighter threshold would re-command several times a second on noise
    // alone. A few percent of bit budget is not worth a mid-stream call.
    static constexpr double kCommandHysteresis = 0.08;

    uint32_t configured_fps_;
    double multiplier_ = 1.0;
    uint32_t commanded_bps_ = 0;
};

} // namespace krg
