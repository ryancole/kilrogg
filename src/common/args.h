#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdlib>

namespace krg {

// A whole number off the command line, in range: true with `out` set, false for
// anything else. Both binaries take numbers from argv and hand them straight to
// arithmetic that has a width, which is where this stops being a formality.
//
// Every one of these was silent, and each left the program announcing one value
// and using another:
//
//   --bitrate 5000   printed "5000 Mbit/s" and wrapped to 705, because
//                    `mbps * 1'000'000` is a uint32
//   --bitrate -5     came through atoi as 4294967291, announced that, and ran
//                    at 4290
//   --port 99999     truncated into a uint16 and listened on 34463
//
// The number reported and the number used disagreeing is the part with no way
// back: every figure afterwards is measured against something nobody chose. So
// the answer to a value that will not fit is to refuse it, not to fold it into
// the nearest legal one — a bitrate quietly becoming a different bitrate is not
// something any later log line would make obvious.
//
// The whole argument has to be the number. Trailing text is a refusal rather
// than something to parse up to, since `--port 80x` is a typo either way.
inline bool parse_uint(const char* text, uint32_t lo, uint32_t hi, uint32_t& out) {
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE) return false;
    if (value < lo || value > hi) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

} // namespace krg
