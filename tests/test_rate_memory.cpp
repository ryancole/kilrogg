#include <doctest/doctest.h>

#include "sender/rate_memory.h"

using namespace krg;

namespace {

using Clock = RateMemory::Clock;
constexpr uint32_t kCeiling = 40'000'000;

// A fixed origin, so the hour-long expiry can be walked through without an
// hour going by.
const Clock::time_point kT0{};

Clock::time_point at(int seconds) { return kT0 + std::chrono::seconds(seconds); }

} // namespace

TEST_CASE("a client nothing is known about recalls nothing") {
    RateMemory m;
    CHECK(m.recall("10.0.0.5", kCeiling, kT0) == 0);
}

TEST_CASE("a client back within the minute gets exactly the link it had") {
    RateMemory m;
    m.remember("10.0.0.5", 12'000'000, kT0);
    CHECK(m.recall("10.0.0.5", kCeiling, kT0) == 12'000'000);
    CHECK(m.recall("10.0.0.5", kCeiling, at(59)) == 12'000'000);
    CHECK(m.recall("10.0.0.5", kCeiling, at(60)) == 12'000'000);
}

TEST_CASE("a reading older than ten minutes is forgotten rather than trusted") {
    RateMemory m;
    m.remember("10.0.0.5", 12'000'000, kT0);
    CHECK(m.recall("10.0.0.5", kCeiling, at(600)) == 0);
    CHECK(m.recall("10.0.0.5", kCeiling, at(6000)) == 0);
}

TEST_CASE("in between, the recalled rate relaxes toward the ceiling") {
    RateMemory m;
    m.remember("10.0.0.5", 10'000'000, kT0);
    // Halfway through the 60 s to 600 s window: half the distance from what was
    // measured to the ceiling. A rate learned on a bad afternoon should not pin
    // the picture down for the evening.
    CHECK(m.recall("10.0.0.5", kCeiling, at(330)) == 25'000'000);
    // And the relaxation is monotonic across the window.
    uint32_t previous = 10'000'000;
    for (int t = 60; t < 600; t += 30) {
        const uint32_t now = m.recall("10.0.0.5", kCeiling, at(t));
        CHECK(now >= previous);
        CHECK(now <= kCeiling);
        previous = now;
    }
}

TEST_CASE("a remembered rate above the ceiling is clamped to it") {
    RateMemory m;
    // --bitrate was lowered between sessions; the old reading is not a licence
    // to exceed the new ceiling.
    m.remember("10.0.0.5", 100'000'000, kT0);
    CHECK(m.recall("10.0.0.5", kCeiling, kT0) == kCeiling);
}

TEST_CASE("a second reading for the same client replaces the first") {
    RateMemory m;
    m.remember("10.0.0.5", 12'000'000, kT0);
    m.remember("10.0.0.5", 6'000'000, at(10));
    CHECK(m.recall("10.0.0.5", kCeiling, at(10)) == 6'000'000);
    // And it renews the clock, so the entry is fresh from the newer reading.
    CHECK(m.recall("10.0.0.5", kCeiling, at(69)) == 6'000'000);
}

TEST_CASE("clients are told apart by address") {
    RateMemory m;
    m.remember("10.0.0.5", 12'000'000, kT0);
    m.remember("10.0.0.6", 6'000'000, kT0);
    CHECK(m.recall("10.0.0.5", kCeiling, kT0) == 12'000'000);
    CHECK(m.recall("10.0.0.6", kCeiling, kT0) == 6'000'000);
    CHECK(m.recall("10.0.0.7", kCeiling, kT0) == 0);
}

TEST_CASE("a client whose address could not be read is not remembered") {
    RateMemory m;
    // peer_address() returns empty when getpeername fails; remembering that
    // would hand one unknown client's link to the next.
    m.remember("", 6'000'000, kT0);
    CHECK(m.recall("", kCeiling, kT0) == 0);
}

TEST_CASE("the table is bounded, evicting the oldest entry") {
    RateMemory m;
    for (int i = 0; i < 9; ++i) {
        m.remember("10.0.0." + std::to_string(i), 6'000'000, at(i));
    }
    CHECK(m.recall("10.0.0.0", kCeiling, at(9)) == 0); // evicted
    for (int i = 1; i < 9; ++i) {
        CHECK(m.recall("10.0.0." + std::to_string(i), kCeiling, at(9)) == 6'000'000);
    }
}
