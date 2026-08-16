#include <doctest/doctest.h>

#include "sender/rate_control.h"

using namespace krg;

namespace {
constexpr uint32_t kFps = 100;    // the rate the encoder was configured for
constexpr uint32_t kTarget = 40'000'000; // what rate control wants on the wire
} // namespace

TEST_CASE("content keeping up with the display is not corrected at all") {
    FrameBudget b(kFps);
    CHECK(b.command(kTarget, 100, 1.0) == kTarget);
    CHECK(b.multiplier() == doctest::Approx(1.0));
}

TEST_CASE("a few missed submission slots are jitter, not a shortfall") {
    FrameBudget b(kFps);
    // 95% of the configured rate. The gap below 1.0 is for exactly this.
    CHECK(b.command(kTarget, 95, 1.0) == kTarget);
    CHECK(b.multiplier() == doctest::Approx(1.0));
}

TEST_CASE("content at half the display's rate climbs to 2x a step at a time") {
    FrameBudget b(kFps);
    // Half the frames the encoder budgeted for means half the bitrate spent,
    // so the command has to double — but gradually, because a command that
    // turns out too high is paid for in a queue overflow.
    CHECK(b.command(kTarget, 50, 1.0) == 50'000'000); // 1.25x
    CHECK(b.command(kTarget, 50, 1.0) == 60'000'000); // 1.50x
    CHECK(b.command(kTarget, 50, 1.0) == 70'000'000); // 1.75x
    CHECK(b.command(kTarget, 50, 1.0) == 80'000'000); // 2.00x
    // And settles there rather than overshooting.
    CHECK(b.command(kTarget, 50, 1.0) == 80'000'000);
    CHECK(b.multiplier() == doctest::Approx(2.0));
}

TEST_CASE("the correction falls the moment content speeds back up") {
    FrameBudget b(kFps);
    for (int i = 0; i < 8; ++i) b.command(kTarget, 50, 1.0);
    REQUIRE(b.multiplier() == doctest::Approx(2.0));

    // Down at once: a stale multiplier does its damage in exactly this case,
    // and one interval of it is enough.
    CHECK(b.command(kTarget, 100, 1.0) == kTarget);
    CHECK(b.multiplier() == doctest::Approx(1.0));
}

TEST_CASE("the correction is capped at 3x") {
    FrameBudget b(kFps);
    // A tenth of the display's rate would want 10x.
    for (int i = 0; i < 40; ++i) b.command(kTarget, 10, 1.0);
    CHECK(b.multiplier() == doctest::Approx(FrameBudget::kMaxMultiplier));
    CHECK(b.command(kTarget, 10, 1.0) == 120'000'000);
}

TEST_CASE("a still screen holds the last multiplier rather than recomputing from stillness") {
    FrameBudget b(kFps);
    b.command(kTarget, 50, 1.0);
    b.command(kTarget, 50, 1.0);
    const double held = b.multiplier();
    REQUIRE(held == doctest::Approx(1.5));

    // Below a tenth of the configured rate there is nothing on screen to spend
    // a larger budget on, and the sample would be mostly noise. A screen that
    // starts moving again should not find a number derived from its stillness.
    for (int i = 0; i < 10; ++i) b.command(kTarget, 1, 1.0);
    CHECK(b.multiplier() == doctest::Approx(held));
}

TEST_CASE("the commanded rate does not move for a change too small to be worth a call into the MFT") {
    FrameBudget b(kFps);
    REQUIRE(b.command(kTarget, 100, 1.0) == kTarget);
    // 2.5% — inside the hysteresis, so the MFT is left alone.
    CHECK(b.command(41'000'000, 100, 1.0) == kTarget);
    // 12.5% — worth saying.
    CHECK(b.command(45'000'000, 100, 1.0) == 45'000'000);
}

TEST_CASE("an interval of no length leaves the multiplier where it was") {
    FrameBudget b(kFps);
    b.command(kTarget, 50, 1.0);
    const double held = b.multiplier();
    CHECK(b.command(kTarget, 0, 0.0) == 50'000'000);
    CHECK(b.multiplier() == doctest::Approx(held));
}

TEST_CASE("a budget configured for no frame rate at all cannot divide by it") {
    FrameBudget b(0);
    CHECK(b.command(kTarget, 50, 1.0) == kTarget);
    CHECK(b.multiplier() == doctest::Approx(1.0));
}

TEST_CASE("the headroom an encoder is built with covers the largest correction") {
    // The two have to agree: an encoder built for the ceiling alone clamps the
    // correction away silently, which is the failure this pairing exists to
    // avoid. 3x of 40 is what a 33 fps game on a 100 Hz panel would ask for.
    FrameBudget b(kFps);
    for (int i = 0; i < 40; ++i) b.command(kTarget, 10, 1.0);
    CHECK(b.command(kTarget, 10, 1.0) <= headroom_bps(kTarget));
}
