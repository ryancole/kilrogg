#include <doctest/doctest.h>

#include "sender/frame_pacer.h"

using namespace krg;

namespace {

using Clock = FramePacer::Clock;

constexpr uint32_t kFps = 100;
constexpr auto kInterval = std::chrono::microseconds(10'000); // 1/100 s
const Clock::time_point kT0{};

// The flush grace in the intervals everything below counts in, so that changing
// either constant fails here rather than quietly changing what these cases are
// about.
constexpr int kGraceIntervals = 3;
static_assert(FramePacer::kFlushGrace == kInterval * kGraceIntervals);

Clock::time_point at(int intervals, std::chrono::microseconds offset = {}) {
    return kT0 + kInterval * intervals + offset;
}

// A pacer with one frame already delivered and submitted at kT0, which is the
// state most of the interesting behaviour starts from.
FramePacer running() {
    FramePacer p(kFps, kT0);
    p.on_frame();
    REQUIRE(p.take_slot(kT0, true, 0));
    return p;
}

// Depths, named so the cases below read as what they mean rather than as
// numbers: the encoder has given everything back, or it still has something.
constexpr int kIdle = 0;
constexpr int kBusy = 1;

} // namespace

TEST_CASE("the first frame goes in as soon as it arrives") {
    FramePacer p(kFps, kT0);
    p.on_frame();
    CHECK(p.take_slot(kT0, true, kIdle));
}

TEST_CASE("a frame arriving early is held until its slot comes round") {
    FramePacer p = running();
    p.on_frame();
    // Capture polls with a zero timeout, so on a fast desktop the next frame is
    // here long before the encoder wants it.
    CHECK_FALSE(p.take_slot(at(0, std::chrono::microseconds(1)), true, kIdle));
    CHECK_FALSE(p.take_slot(at(1, std::chrono::microseconds(-1)), true, kIdle));
    CHECK(p.take_slot(at(1), true, kIdle));
}

TEST_CASE("a frame in hand goes in whatever the encoder is still holding") {
    // The depth only ever decides a *duplicate*. Real content is never held
    // back on account of the encoder being busy — that is what the mailbox and
    // the slot are for.
    FramePacer p = running();
    p.on_frame();
    CHECK(p.take_slot(at(1), true, 4));
}

TEST_CASE("slots advance by exactly one interval while frames keep arriving") {
    FramePacer p = running();
    for (int i = 1; i <= 20; ++i) {
        p.on_frame();
        CHECK_FALSE(p.take_slot(at(i, std::chrono::microseconds(-1)), true, kIdle));
        CHECK(p.take_slot(at(i), true, kIdle));
    }
}

TEST_CASE("the wait is however long is left of the slot") {
    FramePacer p = running();
    p.on_frame();
    // A tenth of the way into the interval leaves nine tenths of it.
    CHECK(p.wait_for(at(0, std::chrono::microseconds(1'000))) ==
          std::chrono::microseconds(9'000));
    CHECK(p.wait_for(at(0, std::chrono::microseconds(9'999))) ==
          std::chrono::microseconds(1));
}

TEST_CASE("the wait never exceeds the quiet poll, however far off the slot is") {
    // The caller's control and stats intervals only run between waits, so this
    // is what bounds how late they can be.
    FramePacer slow(1, kT0); // 1 fps: a full second between slots
    slow.on_frame();
    REQUIRE(slow.take_slot(kT0, true, kIdle));
    slow.on_frame();
    CHECK(slow.wait_for(kT0) == FramePacer::kQuietPoll);
}

TEST_CASE("with nothing in hand the wait is the quiet poll, not the slot") {
    FramePacer p = running();
    // Nothing has arrived since, so there is no slot to wait for — only the
    // poll that keeps the loop responsive on a static screen.
    CHECK(p.wait_for(kT0) == FramePacer::kQuietPoll);
    CHECK(p.wait_for(at(5)) == FramePacer::kQuietPoll);
}

TEST_CASE("falling behind does not produce a burst of catch-up frames") {
    FramePacer p = running();
    p.on_frame();
    // The loop stalled for ten frames' worth of time — a capture that went away
    // for a moment, or an encoder that took too long.
    REQUIRE(p.take_slot(at(10), true, kIdle));
    // The next slot is one interval from now, not ten slots' worth of arrears
    // to be worked through as fast as frames can be found.
    CHECK_FALSE(p.take_slot(at(10), true, kIdle));
    p.on_frame();
    CHECK_FALSE(p.take_slot(at(10, std::chrono::microseconds(1)), true, kIdle));
    CHECK(p.take_slot(at(11), true, kIdle));
}

TEST_CASE("an encoder holding nothing is never asked to flush") {
    // Depth 0 means low-latency mode is genuinely in force and output is 1:1
    // with input. A static screen then produces no traffic at all, however long
    // it stays static.
    FramePacer p = running();
    for (int i = 1; i <= 100; ++i) {
        CHECK_FALSE(p.take_slot(at(i), true, kIdle));
    }
}

TEST_CASE("a frame still being encoded is not a frame being held") {
    // The case this whole grace period exists for. At 175 Hz the next slot
    // comes round in 5.7 ms and an encode takes 4-8 ms, so the encoder is
    // nearly always still busy at the tick right after a submission. Reading
    // that as a stuck frame is what made 11-14% of submissions duplicates.
    FramePacer p = running();
    for (int i = 1; i < kGraceIntervals; ++i) {
        CHECK_FALSE(p.take_slot(at(i), true, kBusy));
    }
}

TEST_CASE("an encoder that lets go inside the grace period is never pushed") {
    // Busy for a while, then done — which is every ordinary frame. Nothing is
    // ever sent twice.
    FramePacer p = running();
    CHECK_FALSE(p.take_slot(at(1), true, kBusy));
    CHECK_FALSE(p.take_slot(at(2), true, kBusy));
    for (int i = 3; i <= 100; ++i) {
        CHECK_FALSE(p.take_slot(at(i), true, kIdle));
    }
}

TEST_CASE("an encoder still holding after the grace gets its duplicate") {
    // The case the re-submit exists for: an MFT that emits frame N only once
    // N+1 arrives sits on it indefinitely, and no amount of waiting helps.
    FramePacer p = running();
    CHECK_FALSE(p.take_slot(at(kGraceIntervals - 1), true, kBusy));
    CHECK(p.take_slot(at(kGraceIntervals), true, kBusy));
}

TEST_CASE("a second duplicate waits out the grace again, not just a slot") {
    // A re-submit is itself a submission, so the encoder gets the same benefit
    // of the doubt after it as before it. Otherwise one stuck frame turns into
    // a duplicate every slot for as long as the budget lasts.
    FramePacer p = running();
    REQUIRE(p.take_slot(at(kGraceIntervals), true, kBusy));
    CHECK_FALSE(p.take_slot(at(kGraceIntervals + 1), true, kBusy));
    CHECK_FALSE(p.take_slot(at(2 * kGraceIntervals - 1), true, kBusy));
    CHECK(p.take_slot(at(2 * kGraceIntervals), true, kBusy));
}

TEST_CASE("a depth that is wrong forever costs a bounded number of duplicates") {
    // pipeline_depth() is submitted-minus-emitted, and a ProcessOutput that
    // fails leaves emitted permanently behind — so the reading can be not just
    // wrong but wrong forever. The ceiling is what stops that becoming a
    // standing bandwidth cost: one burst of motion, eight duplicates, quiet.
    FramePacer p = running();
    int sent = 0;
    for (int i = 1; i <= 100; ++i) {
        if (p.take_slot(at(i * kGraceIntervals), true, 1'000'000)) ++sent;
    }
    CHECK(sent == FramePacer::kMaxFlushResubmits);
}

TEST_CASE("a negative depth is read as nothing held rather than as a budget") {
    FramePacer p = running();
    CHECK_FALSE(p.take_slot(at(kGraceIntervals), true, -5));
    CHECK_FALSE(p.take_slot(at(10 * kGraceIntervals), true, -5));
}

TEST_CASE("a fresh frame refreshes the re-submit budget") {
    FramePacer p = running();
    int sent = 0;
    for (int i = 1; i <= 100; ++i) {
        if (p.take_slot(at(i * kGraceIntervals), true, kBusy)) ++sent;
    }
    REQUIRE(sent == FramePacer::kMaxFlushResubmits);

    // Content starts moving again, and the budget is a per-quiet-stretch thing
    // rather than something spent once for the whole session.
    p.on_frame();
    CHECK(p.take_slot(at(101 * kGraceIntervals), true, kBusy)); // the frame itself
    CHECK(p.take_slot(at(102 * kGraceIntervals), true, kBusy)); // and a flush after it
}

TEST_CASE("nothing is submitted before the first frame has ever arrived") {
    FramePacer p(kFps, kT0);
    // The mailbox has produced nothing yet, so there is no texture to hand over
    // however willing the clock is. on_frame() is not called here, because the
    // caller only calls it holding the frame it is announcing.
    for (int i = 0; i <= 20; ++i) {
        CHECK_FALSE(p.take_slot(at(i), false, kBusy));
    }
}

TEST_CASE("a re-submit needs a frame to re-submit, not just budget for one") {
    // have_frame guards this path and only this path: a caller that has lost
    // its texture — a mode change dropped it — must not be told to send it
    // again on the strength of a budget left over from before.
    FramePacer p = running();
    CHECK_FALSE(p.take_slot(at(kGraceIntervals), false, kBusy));
    CHECK_FALSE(p.take_slot(at(2 * kGraceIntervals), false, kBusy));
    // And the budget was not spent on the refusals.
    CHECK(p.take_slot(at(3 * kGraceIntervals), true, kBusy));
}

TEST_CASE("a frame rate of zero does not divide by it") {
    // encoder->fps() should never be 0, but the cost of being wrong here is a
    // crash rather than a bad picture.
    FramePacer p(0, kT0);
    p.on_frame();
    CHECK(p.take_slot(kT0, true, kIdle));
    CHECK(p.wait_for(kT0) == FramePacer::kQuietPoll);
}

TEST_CASE("the pacer keeps the encoder's rate, which is not always the display's") {
    // 2560x1600 at 240 Hz settles on 129 fps in H.264; pacing to 240 would feed
    // the encoder nearly twice what it budgeted for.
    FramePacer p(129, kT0);
    p.on_frame();
    REQUIRE(p.take_slot(kT0, true, kIdle));
    p.on_frame();
    const auto slot = std::chrono::microseconds(1'000'000 / 129);
    CHECK_FALSE(p.take_slot(kT0 + slot - std::chrono::microseconds(1), true, kIdle));
    CHECK(p.take_slot(kT0 + slot, true, kIdle));
}
