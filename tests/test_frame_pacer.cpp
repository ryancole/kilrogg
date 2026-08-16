#include <doctest/doctest.h>

#include "sender/frame_pacer.h"

using namespace krg;

namespace {

using Clock = FramePacer::Clock;

constexpr uint32_t kFps = 100;
constexpr auto kInterval = std::chrono::microseconds(10'000); // 1/100 s
const Clock::time_point kT0{};

Clock::time_point at(int intervals, std::chrono::microseconds offset = {}) {
    return kT0 + kInterval * intervals + offset;
}

// A pacer with one frame already delivered and submitted, which is the state
// most of the interesting behaviour starts from. `depth` is what the encoder
// reported when that frame arrived.
FramePacer running(int depth = 0) {
    FramePacer p(kFps, kT0);
    p.on_frame(depth);
    REQUIRE(p.take_slot(kT0, true));
    return p;
}

} // namespace

TEST_CASE("the first frame goes in as soon as it arrives") {
    FramePacer p(kFps, kT0);
    p.on_frame(0);
    CHECK(p.take_slot(kT0, true));
}

TEST_CASE("a frame arriving early is held until its slot comes round") {
    FramePacer p = running();
    p.on_frame(0);
    // Capture polls with a zero timeout, so on a fast desktop the next frame is
    // here long before the encoder wants it.
    CHECK_FALSE(p.take_slot(at(0, std::chrono::microseconds(1)), true));
    CHECK_FALSE(p.take_slot(at(1, std::chrono::microseconds(-1)), true));
    CHECK(p.take_slot(at(1), true));
}

TEST_CASE("slots advance by exactly one interval while frames keep arriving") {
    FramePacer p = running();
    for (int i = 1; i <= 20; ++i) {
        p.on_frame(0);
        CHECK_FALSE(p.take_slot(at(i, std::chrono::microseconds(-1)), true));
        CHECK(p.take_slot(at(i), true));
    }
}

TEST_CASE("the wait is however long is left of the slot") {
    FramePacer p = running();
    p.on_frame(0);
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
    slow.on_frame(0);
    REQUIRE(slow.take_slot(kT0, true));
    slow.on_frame(0);
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
    p.on_frame(0);
    // The loop stalled for ten frames' worth of time — a capture that went away
    // for a moment, or an encoder that took too long.
    REQUIRE(p.take_slot(at(10), true));
    // The next slot is one interval from now, not ten slots' worth of arrears
    // to be worked through as fast as frames can be found.
    CHECK_FALSE(p.take_slot(at(10), true));
    p.on_frame(0);
    CHECK_FALSE(p.take_slot(at(10, std::chrono::microseconds(1)), true));
    CHECK(p.take_slot(at(11), true));
}

TEST_CASE("an encoder holding nothing is never asked to flush") {
    // Depth 0 means low-latency mode is genuinely in force and output is 1:1
    // with input. A static screen then produces no traffic at all.
    FramePacer p = running(0);
    for (int i = 1; i <= 20; ++i) {
        CHECK_FALSE(p.take_slot(at(i), true));
    }
}

TEST_CASE("a held-back frame is re-submitted as many times as the encoder is holding") {
    // Three frames stuck inside the MFT take three more pushes to come out.
    FramePacer p = running(3);
    CHECK(p.take_slot(at(1), true));
    CHECK(p.take_slot(at(2), true));
    CHECK(p.take_slot(at(3), true));
    // And then the screen is static and nothing more goes out.
    CHECK_FALSE(p.take_slot(at(4), true));
    CHECK_FALSE(p.take_slot(at(5), true));
}

TEST_CASE("re-submits are paced like everything else, not spun through") {
    FramePacer p = running(3);
    CHECK_FALSE(p.take_slot(at(0, std::chrono::microseconds(1)), true));
    CHECK(p.take_slot(at(1), true));
    CHECK_FALSE(p.take_slot(at(1, std::chrono::microseconds(1)), true));
}

TEST_CASE("a wildly wrong depth costs a bounded number of duplicate frames") {
    // pipeline_depth() is submitted-minus-emitted, and a ProcessOutput that
    // fails leaves emitted permanently behind — so the reading can be not just
    // wrong but wrong forever. The ceiling is what stops that becoming a
    // standing bandwidth cost: one burst of motion, eight duplicates, quiet.
    FramePacer p = running(1'000'000);
    int sent = 0;
    for (int i = 1; i <= 100; ++i) {
        if (p.take_slot(at(i), true)) ++sent;
    }
    CHECK(sent == FramePacer::kMaxFlushResubmits);
}

TEST_CASE("a negative depth is read as nothing held rather than as a budget") {
    FramePacer p = running(-5);
    CHECK_FALSE(p.take_slot(at(1), true));
}

TEST_CASE("a fresh frame refreshes the re-submit budget") {
    FramePacer p = running(2);
    CHECK(p.take_slot(at(1), true));
    CHECK(p.take_slot(at(2), true));
    CHECK_FALSE(p.take_slot(at(3), true));

    p.on_frame(2);
    CHECK(p.take_slot(at(4), true)); // the frame itself
    CHECK(p.take_slot(at(5), true)); // and its two flushes
    CHECK(p.take_slot(at(6), true));
    CHECK_FALSE(p.take_slot(at(7), true));
}

TEST_CASE("nothing is submitted before the first frame has ever arrived") {
    FramePacer p(kFps, kT0);
    // The mailbox has produced nothing yet, so there is no texture to hand over
    // however willing the clock is. on_frame() is not called here, because the
    // caller only calls it holding the frame it is announcing.
    for (int i = 0; i <= 20; ++i) {
        CHECK_FALSE(p.take_slot(at(i), false));
    }
}

TEST_CASE("a re-submit needs a frame to re-submit, not just budget for one") {
    // have_frame guards this path and only this path: a caller that has lost
    // its texture — a mode change dropped it — must not be told to send it
    // again on the strength of a budget left over from before.
    FramePacer p = running(3);
    CHECK_FALSE(p.take_slot(at(1), false));
    CHECK_FALSE(p.take_slot(at(2), false));
    // And the budget was not spent on the refusals.
    CHECK(p.take_slot(at(3), true));
}

TEST_CASE("a frame rate of zero does not divide by it") {
    // encoder->fps() should never be 0, but the cost of being wrong here is a
    // crash rather than a bad picture.
    FramePacer p(0, kT0);
    p.on_frame(0);
    CHECK(p.take_slot(kT0, true));
    CHECK(p.wait_for(kT0) == FramePacer::kQuietPoll);
}

TEST_CASE("the pacer keeps the encoder's rate, which is not always the display's") {
    // 2560x1600 at 240 Hz settles on 129 fps in H.264; pacing to 240 would feed
    // the encoder nearly twice what it budgeted for.
    FramePacer p(129, kT0);
    p.on_frame(0);
    REQUIRE(p.take_slot(kT0, true));
    p.on_frame(0);
    const auto slot = std::chrono::microseconds(1'000'000 / 129);
    CHECK_FALSE(p.take_slot(kT0 + slot - std::chrono::microseconds(1), true));
    CHECK(p.take_slot(kT0 + slot, true));
}
