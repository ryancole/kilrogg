#include <doctest/doctest.h>

#include "common/args.h"

using namespace krg;

namespace {

// The bounds the two binaries actually use, so the cases below are the ones
// that reach the arithmetic rather than invented ones.
constexpr uint32_t kBitrateLo = 1, kBitrateHi = 1000;
constexpr uint32_t kPortLo = 1, kPortHi = 65535;

// Refused values must leave the caller's variable alone: every call site
// declares it uninitialized-but-for-a-default and reads it only on success, so
// a parser that wrote a partial answer before failing would be worse than one
// that failed.
bool refused(const char* text, uint32_t lo, uint32_t hi) {
    uint32_t out = 12345;
    const bool ok = parse_uint(text, lo, hi, out);
    return !ok && out == 12345;
}

uint32_t parsed(const char* text, uint32_t lo, uint32_t hi) {
    uint32_t out = 0;
    REQUIRE(parse_uint(text, lo, hi, out));
    return out;
}

} // namespace

TEST_CASE("a whole number in range is the number") {
    CHECK(parsed("40", kBitrateLo, kBitrateHi) == 40);
    CHECK(parsed("47800", kPortLo, kPortHi) == 47800);
}

TEST_CASE("both ends of the range are inside it") {
    CHECK(parsed("1", kBitrateLo, kBitrateHi) == 1);
    CHECK(parsed("1000", kBitrateLo, kBitrateHi) == 1000);
    CHECK(parsed("65535", kPortLo, kPortHi) == 65535);
}

TEST_CASE("a number past the range is refused rather than clamped to it") {
    // The case this exists for: 5000 megabits used to survive parsing and wrap
    // to 705 at `mbps * 1'000'000`, having already been announced as 5000.
    CHECK(refused("5000", kBitrateLo, kBitrateHi));
    CHECK(refused("1001", kBitrateLo, kBitrateHi));
    // And this one truncated into a uint16 and listened on 34463.
    CHECK(refused("99999", kPortLo, kPortHi));
    CHECK(refused("65536", kPortLo, kPortHi));
}

TEST_CASE("a number below the range is refused") {
    CHECK(refused("0", kBitrateLo, kBitrateHi));
    CHECK(refused("0", kPortLo, kPortHi));
}

TEST_CASE("a negative is refused rather than read as an enormous unsigned") {
    // atoi gave -5 to a uint32 cast, which is 4294967291, which the sender then
    // printed as its bitrate. Nothing about that reads as a rejected argument.
    CHECK(refused("-5", kBitrateLo, kBitrateHi));
    CHECK(refused("-1", kPortLo, kPortHi));
    CHECK(refused("-2147483648", kBitrateLo, kBitrateHi));
}

TEST_CASE("something that is not a number at all is refused") {
    CHECK(refused("abc", kBitrateLo, kBitrateHi));
    CHECK(refused("", kBitrateLo, kBitrateHi));
    CHECK(refused("--bitrate", kBitrateLo, kBitrateHi));
}

TEST_CASE("the whole argument has to be the number, not a prefix of it") {
    // A parser that stopped at the first non-digit would read these as 40 and
    // 80, which is a typo silently doing something.
    CHECK(refused("40x", kBitrateLo, kBitrateHi));
    CHECK(refused("80x", kPortLo, kPortHi));
    CHECK(refused("40 60", kBitrateLo, kBitrateHi));
    CHECK(refused("1e6", kBitrateLo, kBitrateHi));
    CHECK(refused("40.5", kBitrateLo, kBitrateHi));
}

TEST_CASE("a number too large for the arithmetic itself is refused") {
    // Past long long, so strtoll saturates and sets ERANGE. Without that check
    // it would come back as LLONG_MAX and be judged on the range alone, which
    // happens to give the same answer here — but would not if the bound were
    // ever raised to the top of the type.
    CHECK(refused("99999999999999999999", kBitrateLo, kBitrateHi));
    CHECK(refused("99999999999999999999", 0, UINT32_MAX));
}

TEST_CASE("the full uint32 range is representable") {
    // Nothing asks for this today, but a bound that cannot express the type's
    // own maximum is a trap for whoever raises one.
    CHECK(parsed("4294967295", 0, UINT32_MAX) == UINT32_MAX);
    CHECK(parsed("0", 0, UINT32_MAX) == 0);
    CHECK(refused("4294967296", 0, UINT32_MAX));
}
