#pragma once
#include <cstdint>
#include <vector>

namespace krg {

struct Rect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

// Full desktop image (BGRA8, tightly packed) plus the regions that changed
// since the previous frame. Because `pixels` is always the complete image,
// dropping a frame is safe as long as its dirty rects are merged into the
// frame that replaces it.
struct Frame {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // width * height * 4
    std::vector<Rect> rects;
    uint32_t id = 0;
};

} // namespace krg
