#pragma once
#include <cstdint>
#include <vector>

namespace krg {

struct Rect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

// Trims a rect to a frame of `w` x `h`. Dirty rects come from DXGI, whose
// origin can sit outside the desktop image the rect is being applied to — a
// move rect straddling the edge of the display, most often — and a rect that
// hangs over the edge would be read from past the end of the pixel buffer.
// A rect that starts outside altogether collapses to nothing rather than being
// clamped inward, since there is no part of it the frame actually contains.
inline Rect clamp_rect(Rect r, uint32_t w, uint32_t h) {
    if (r.x >= w || r.y >= h) return {0, 0, 0, 0};
    if (r.x + r.w > w) r.w = w - r.x;
    if (r.y + r.h > h) r.h = h - r.y;
    return r;
}

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
