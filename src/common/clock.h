#pragma once
#include <windows.h>

#include <cstdint>

namespace krg {

// Microseconds from a per-machine monotonic clock (QPC). Both ends stamp
// timings with this; the epochs differ between machines, which is what the
// receiver's ping/pong clock-offset estimate exists to cancel out.
inline int64_t qpc_frequency() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

// Converts a raw QPC count — DXGI's frame statistics report scanout times as
// one — to the same microsecond scale as now_us().
inline int64_t qpc_to_us(int64_t counter) {
    const int64_t freq = qpc_frequency();
    // Split to keep the multiply from overflowing on a long-running machine.
    return (counter / freq) * 1'000'000 + (counter % freq) * 1'000'000 / freq;
}

inline int64_t now_us() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return qpc_to_us(c.QuadPart);
}

} // namespace krg
