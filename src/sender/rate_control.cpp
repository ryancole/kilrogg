#include "sender/rate_control.h"

#include <algorithm>

namespace krg {
namespace {

// One congestion event should produce one cut, not one per interval: the
// encoder needs a few frames to act on a new target and the queue needs time
// to drain at it. Without this window a single Wi-Fi retransmit burst walks
// the rate all the way to the floor.
constexpr double kCutCooldownSecs = 0.75;

// Clean intervals required before probing upward. Recovery is deliberately
// slower than the cut: guessing high costs a backlog flush, which costs an
// IDR, which is the most expensive thing that can go on the wire.
constexpr int kCleanIntervalsBeforeProbe = 5;

// Consecutive deep samples before depth alone cuts the rate. One is not enough:
// the encoder hands a frame over in a single burst, so a queue caught mid-drain
// looks deep without the link being behind at all.
constexpr int kDeepIntervalsBeforeCut = 2;

constexpr double kFlushCutFactor = 0.75;
constexpr double kDepthCutFactor = 0.85;
// Fraction of what the link was measured to carry to aim for after a flush.
// Landing exactly on the measurement just overflows again more slowly.
constexpr double kMeasuredHeadroom = 0.9;

uint32_t scale(uint32_t bps, double factor) {
    return static_cast<uint32_t>(bps * factor);
}

} // namespace

RateControl::RateControl(uint32_t ceiling_bps, uint32_t floor_bps, uint32_t start_bps)
    : ceiling_bps_(ceiling_bps),
      floor_bps_(std::min(floor_bps, ceiling_bps)),
      // Roughly a sixteenth of the ceiling per probe: from the floor back to a
      // 40 Mbit/s ceiling takes some fifteen seconds of clean link.
      step_bps_(std::max<uint32_t>(500'000, ceiling_bps / 16)),
      target_bps_(start_bps ? std::clamp(start_bps, floor_bps_, ceiling_bps_) : ceiling_bps),
      // There has been no cut to cool down from, and a link that is over
      // capacity is usually over capacity from the first frame — starting the
      // window armed would sit out the first three-quarters of a second of
      // exactly the congestion this exists to answer.
      secs_since_cut_(kCutCooldownSecs) {}

RateControl::Decision RateControl::update(const Sample& s) {
    if (s.secs <= 0) return {};
    secs_since_cut_ += s.secs;

    const uint64_t measured_bps = static_cast<uint64_t>(s.bytes_sent * 8.0 / s.secs);
    const bool flushed = s.backlog_flushes > 0;
    // Half the budget still queued at the end of the interval is video the
    // viewer is already waiting on — not yet an overflow, but heading for one.
    const bool deep = s.queue_capacity && s.queued_bytes * 2 > s.queue_capacity;
    deep_intervals_ = deep ? deep_intervals_ + 1 : 0;

    if (flushed || deep) clean_intervals_ = 0;

    // A flush is a loss that already happened, so it counts on its own; depth
    // has to persist, since one deep sample can still be a burst caught at an
    // unlucky moment.
    if (flushed || deep_intervals_ >= kDeepIntervalsBeforeCut) {
        if (secs_since_cut_ < kCutCooldownSecs) return {};
        // Everything asked for went out during the interval, so capacity is
        // not what filled the queue. Cutting here would trade picture quality
        // for nothing.
        if (measured_bps >= target_bps_) return {};

        uint32_t next = scale(target_bps_, flushed ? kFlushCutFactor : kDepthCutFactor);
        if (flushed && measured_bps) {
            // A backed-up queue keeps the send thread busy for the whole
            // interval, so the bytes that made it out are a direct reading of
            // the link. Drop to just under it rather than stepping down
            // blindly and taking several intervals to get there.
            next = std::min<uint32_t>(next, scale(static_cast<uint32_t>(measured_bps),
                                                  kMeasuredHeadroom));
        }
        next = std::max(next, floor_bps_);
        if (next >= target_bps_) return {}; // already at the floor
        secs_since_cut_ = 0;
        deep_intervals_ = 0;
        target_bps_ = next;
        return {target_bps_, true};
    }

    // An interval that barely used the link says nothing about how fast it is:
    // a static screen sends nothing at all. Probing upward on that evidence
    // would ramp back to the ceiling during a quiet stretch and then blow the
    // queue the moment the picture starts moving again.
    if (measured_bps * 2 < target_bps_) return {};
    // Draining, but not with room to spare. Hold station.
    if (s.queue_capacity && s.queued_bytes * 8 > s.queue_capacity) {
        clean_intervals_ = 0;
        return {};
    }

    if (++clean_intervals_ < kCleanIntervalsBeforeProbe) return {};
    clean_intervals_ = 0;
    if (target_bps_ >= ceiling_bps_) return {};
    target_bps_ = std::min(ceiling_bps_, target_bps_ + step_bps_);
    return {target_bps_, false};
}

} // namespace krg
