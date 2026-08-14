#pragma once
#include <cstdint>

namespace krg {

constexpr uint32_t kMagic = 0x3147524B; // "KRG1" little-endian
constexpr uint16_t kDefaultPort = 47800;

// Wire format (little-endian, Windows-only):
//   on connect:  Hello
//   per frame:   FrameHeader, then rect_count x (RectHeader + comp_size bytes)
// Pixels are BGRA8. A full-frame rect acts as the keyframe.
#pragma pack(push, 1)
struct Hello {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
};

struct FrameHeader {
    uint32_t frame_id;
    uint32_t rect_count;
};

struct RectHeader {
    uint32_t x, y, w, h;
    uint32_t comp_size; // LZ4-compressed payload bytes following this header
    uint32_t raw_size;  // w * h * 4
};
#pragma pack(pop)

} // namespace krg
