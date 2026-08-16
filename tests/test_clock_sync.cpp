#include <doctest/doctest.h>

#include "receiver/clock_sync.h"

using namespace krg;

namespace {
// The window after which the best sample expires, from clock_sync.h.
constexpr int64_t kWindowUs = 30'000'000;
} // namespace

TEST_CASE("nothing is claimed about the offset until a probe lands") {
    ClockSync c;
    CHECK_FALSE(c.valid());
    // The overlay prints "--" rather than a fabricated number while this holds.
}

TEST_CASE("one round trip puts the sender's clock in receiver terms") {
    ClockSync c;
    // Sent at 1000 on the receiver's clock, answered at 5000 on the sender's,
    // back at 1200. The midpoint of the round trip is receiver-time 1100, and
    // that is the instant the sender's 5000 is assumed to name.
    c.on_pong(Pong{1000, 5000}, 1200);
    REQUIRE(c.valid());
    CHECK(c.to_local(5000) == 1100);
    CHECK(c.rtt_ms() == doctest::Approx(0.2));
    // The mapping is a shift, so elapsed sender time survives it intact.
    CHECK(c.to_local(6000) - c.to_local(5000) == 1000);
}

TEST_CASE("a more queued round trip is ignored while the best one is still fresh") {
    ClockSync c;
    c.on_pong(Pong{1000, 5000}, 1200); // rtt 200
    const int64_t clean = c.to_local(5000);

    // Ten times the round trip: mostly queueing, and it would drag the estimate
    // with it. Well inside the window, so the earlier sample stands.
    c.on_pong(Pong{2000, 9000}, 4000); // rtt 2000
    CHECK(c.to_local(5000) == clean);
    CHECK(c.rtt_ms() == doctest::Approx(0.2));
}

TEST_CASE("a quieter round trip wins immediately") {
    ClockSync c;
    c.on_pong(Pong{1000, 5000}, 1200); // rtt 200
    c.on_pong(Pong{2000, 6050}, 2100); // rtt 100, offset 6050 - 2050 = 4000
    CHECK(c.rtt_ms() == doctest::Approx(0.1));
    CHECK(c.to_local(5000) == 1000);
}

TEST_CASE("the best sample expires, so drift is corrected rather than locked in") {
    ClockSync c;
    const int64_t first_at = 1200;
    c.on_pong(Pong{1000, 5000}, first_at); // rtt 200
    const int64_t before = c.to_local(5000);

    // A worse sample one microsecond short of the window is still refused.
    const int64_t inside = first_at + kWindowUs - 1;
    c.on_pong(Pong{inside - 4'000'000, 44'000'000}, inside); // rtt 4 s
    CHECK(c.to_local(5000) == before);

    // Past it, whatever the next probe says replaces it — a slightly queued
    // sample now beats a pristine one from half an hour ago, because the two
    // machines' clocks have moved apart in the meantime, and that drift is
    // exactly what the later probes are for.
    const int64_t outside = first_at + kWindowUs;
    c.on_pong(Pong{outside - 4'000'000, 44'000'000}, outside); // same bad rtt
    CHECK(c.to_local(5000) != before);
    CHECK(c.valid());
    CHECK(c.rtt_ms() == doctest::Approx(4000.0));
}

TEST_CASE("a pong that appears to arrive before it was sent is discarded") {
    ClockSync c;
    // Nothing legitimate produces this; taking it would poison the offset with
    // a negative round trip.
    c.on_pong(Pong{5000, 9000}, 1000);
    CHECK_FALSE(c.valid());

    c.on_pong(Pong{1000, 5000}, 1200);
    REQUIRE(c.valid());
    const int64_t good = c.to_local(5000);
    c.on_pong(Pong{5000, 9000}, 1000);
    CHECK(c.to_local(5000) == good);
}

TEST_CASE("an instantaneous round trip is a straight offset") {
    ClockSync c;
    // Loopback: no travel time to halve, so the sender's clock reading maps
    // onto the moment it was received.
    c.on_pong(Pong{1000, 7000}, 1000);
    REQUIRE(c.valid());
    CHECK(c.rtt_ms() == doctest::Approx(0.0));
    CHECK(c.to_local(7000) == 1000);
}
