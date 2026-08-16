#include <doctest/doctest.h>

#include "sender/rate_control.h"

using namespace krg;

namespace {

constexpr uint32_t kCeiling = 40'000'000;
constexpr uint32_t kFloor = 3'000'000;
constexpr size_t kCapacity = 1u << 20;
// The sender runs a 200 ms control interval. These use 250 ms instead, for no
// reason but arithmetic: a quarter is exact in binary and a fifth is not, and
// the assertions below compare bitrates that have been through a divide.
constexpr double kInterval = 0.25;

// One interval in which the wire carried `measured_bps` and the queue ended
// `queued_bytes` deep.
RateControl::Sample sample(uint32_t measured_bps, uint64_t flushes = 0, size_t queued_bytes = 0,
                           double secs = kInterval) {
    RateControl::Sample s;
    s.bytes_sent = static_cast<uint64_t>(measured_bps / 8.0 * secs);
    s.backlog_flushes = flushes;
    s.queued_bytes = queued_bytes;
    s.queue_capacity = kCapacity;
    s.secs = secs;
    return s;
}

// Feeds clean intervals until the cut cooldown has expired, asserting none of
// them moves the rate. Used so a test about the second cut is not really a test
// about the cooldown.
void ride_out_cooldown(RateControl& rc) {
    for (int i = 0; i < 5; ++i) {
        CHECK(rc.update(sample(1, 0, 0)).bps == 0);
    }
}

} // namespace

TEST_CASE("a link nothing is known about starts at the ceiling") {
    RateControl rc(kCeiling, kFloor);
    CHECK(rc.target_bps() == kCeiling);
}

TEST_CASE("a recalled starting rate is clamped into [floor, ceiling]") {
    CHECK(RateControl(kCeiling, kFloor, 10'000'000).target_bps() == 10'000'000);
    CHECK(RateControl(kCeiling, kFloor, 100'000'000).target_bps() == kCeiling);
    CHECK(RateControl(kCeiling, kFloor, 1'000'000).target_bps() == kFloor);
    // 0 is "nothing remembered", not "start at zero".
    CHECK(RateControl(kCeiling, kFloor, 0).target_bps() == kCeiling);
}

TEST_CASE("a floor above the ceiling collapses onto the ceiling") {
    RateControl rc(5'000'000, 40'000'000);
    CHECK(rc.target_bps() == 5'000'000);
    // And the cut path cannot then drop below it.
    RateControl::Decision d = rc.update(sample(1'000'000, 1));
    CHECK(d.bps == 0);
}

TEST_CASE("a flush cuts to just under what the wire was measured to carry") {
    RateControl rc(kCeiling, kFloor);
    // 20 Mbit/s got out while the queue overflowed, so the link is 20, not 40.
    RateControl::Decision d = rc.update(sample(20'000'000, 1));
    REQUIRE(d.bps != 0);
    CHECK(d.congested);
    // 90% of the measurement, which is below the blind 0.75 step from 40.
    CHECK(d.bps == 18'000'000);
    CHECK(rc.target_bps() == 18'000'000);
}

TEST_CASE("a flush on a link carrying everything it was asked for does not cut") {
    RateControl rc(kCeiling, kFloor);
    // The loopback case: the encoder handed over a burst and the queue was
    // caught mid-drain. Capacity is not what filled it, so cutting would trade
    // picture quality for nothing.
    CHECK(rc.update(sample(kCeiling, 1)).bps == 0);
    CHECK(rc.target_bps() == kCeiling);
}

TEST_CASE("one congestion event produces one cut, not one per interval") {
    RateControl rc(kCeiling, kFloor);
    REQUIRE(rc.update(sample(20'000'000, 1)).bps == 18'000'000);

    // A burst of flushes inside the 0.75 s cooldown is still the same event:
    // the encoder has not had time to act on the new target, nor the queue to
    // drain at it.
    CHECK(rc.update(sample(1'000'000, 1)).bps == 0);
    CHECK(rc.update(sample(1'000'000, 1)).bps == 0);
    CHECK(rc.target_bps() == 18'000'000);

    // A third interval puts 0.75 s between this and the cut, and a flush counts
    // again.
    RateControl::Decision d = rc.update(sample(1'000'000, 1));
    CHECK(d.bps != 0);
    CHECK(d.congested);
    CHECK(d.bps < 18'000'000);
}

TEST_CASE("depth alone has to persist before it cuts") {
    RateControl rc(kCeiling, kFloor);
    // Over half the budget still queued: heading for an overflow, but one
    // sample can still be a burst caught at an unlucky moment.
    const size_t deep = kCapacity * 3 / 4;
    CHECK(rc.update(sample(20'000'000, 0, deep)).bps == 0);
    CHECK(rc.target_bps() == kCeiling);

    // Twice in a row is the link.
    RateControl::Decision d = rc.update(sample(20'000'000, 0, deep));
    REQUIRE(d.bps != 0);
    CHECK(d.congested);
    // The gentler factor, and no measured-rate clamp: nothing was lost yet.
    CHECK(d.bps == 34'000'000);
}

TEST_CASE("a single deep sample between shallow ones never accumulates") {
    RateControl rc(kCeiling, kFloor);
    const size_t deep = kCapacity * 3 / 4;
    for (int i = 0; i < 6; ++i) {
        CHECK(rc.update(sample(20'000'000, 0, deep)).bps == 0);
        CHECK(rc.update(sample(20'000'000, 0, 0)).bps == 0);
    }
    CHECK(rc.target_bps() == kCeiling);
}

TEST_CASE("the rate probes upward by a sixteenth of the ceiling after five clean intervals") {
    RateControl rc(kCeiling, kFloor, 10'000'000);
    for (int i = 0; i < 4; ++i) {
        CHECK(rc.update(sample(10'000'000)).bps == 0);
    }
    RateControl::Decision d = rc.update(sample(10'000'000));
    REQUIRE(d.bps != 0);
    CHECK_FALSE(d.congested);
    CHECK(d.bps == 12'500'000); // 10 + 40/16
}

TEST_CASE("a quiet interval is not evidence the link got faster") {
    RateControl rc(kCeiling, kFloor, 10'000'000);
    // A static screen sends almost nothing. Probing on that would ramp back to
    // the ceiling during the quiet stretch and blow the queue the moment the
    // picture starts moving again.
    for (int i = 0; i < 20; ++i) {
        CHECK(rc.update(sample(1'000'000)).bps == 0);
    }
    CHECK(rc.target_bps() == 10'000'000);
}

TEST_CASE("a queue that is draining but not empty holds station") {
    RateControl rc(kCeiling, kFloor, 10'000'000);
    const size_t lingering = kCapacity / 4; // past capacity/8, short of deep
    for (int i = 0; i < 20; ++i) {
        CHECK(rc.update(sample(10'000'000, 0, lingering)).bps == 0);
    }
    CHECK(rc.target_bps() == 10'000'000);
}

TEST_CASE("the probe stops at the ceiling") {
    RateControl rc(kCeiling, kFloor, 39'000'000);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(rc.update(sample(39'000'000)).bps == 0);
    }
    CHECK(rc.update(sample(39'000'000)).bps == kCeiling);
    for (int i = 0; i < 20; ++i) {
        CHECK(rc.update(sample(kCeiling)).bps == 0);
    }
    CHECK(rc.target_bps() == kCeiling);
}

TEST_CASE("cuts stop at the floor rather than going under it") {
    RateControl rc(kCeiling, kFloor);
    for (int i = 0; i < 40; ++i) {
        rc.update(sample(100'000, 1));
        ride_out_cooldown(rc);
    }
    CHECK(rc.target_bps() == kFloor);
    // At the floor a further flush is not a decision at all — there is nothing
    // left to give, and re-commanding the same rate is a call into the MFT for
    // no reason.
    CHECK(rc.update(sample(100'000, 1)).bps == 0);
}

TEST_CASE("an interval of zero or negative length is ignored") {
    RateControl rc(kCeiling, kFloor);
    RateControl::Sample s = sample(1'000'000, 1);
    s.secs = 0;
    CHECK(rc.update(s).bps == 0);
    s.secs = -1;
    CHECK(rc.update(s).bps == 0);
    CHECK(rc.target_bps() == kCeiling);
}

TEST_CASE("a sample with no queue capacity cannot be judged deep") {
    RateControl rc(kCeiling, kFloor, 10'000'000);
    RateControl::Sample s = sample(10'000'000, 0, 999'999);
    s.queue_capacity = 0;
    // Nothing to measure the depth against, so the interval counts as clean and
    // five of them still probe.
    for (int i = 0; i < 4; ++i) {
        CHECK(rc.update(s).bps == 0);
    }
    CHECK(rc.update(s).bps == 12'500'000);
}

TEST_CASE("the step never falls below half a megabit, however small the ceiling") {
    RateControl rc(2'000'000, 500'000, 500'000);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(rc.update(sample(500'000)).bps == 0);
    }
    // 2M/16 is 125k, so the floor on the step is what applies.
    CHECK(rc.update(sample(500'000)).bps == 1'000'000);
}

TEST_CASE("the send queue budget is a quarter second of video, with a floor") {
    CHECK(queue_budget_bytes(40'000'000) == 40'000'000 / 32);
    CHECK(queue_budget_bytes(40'000'000) == 1'250'000);
    // Below ~16 Mbit/s the floor keeps the queue larger than a single frame.
    CHECK(queue_budget_bytes(1'000'000) == (512u << 10));
    CHECK(queue_budget_bytes(0) == (512u << 10));
}

TEST_CASE("encoder headroom is the ceiling times the largest frame-budget correction") {
    CHECK(headroom_bps(40'000'000) == 120'000'000);
    // An absurd ceiling saturates rather than wrapping: the encoder is built
    // from this number.
    CHECK(headroom_bps(4'000'000'000u) == UINT32_MAX);
}
