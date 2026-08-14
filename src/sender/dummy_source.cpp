#include "sender/dummy_source.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace krg {

namespace {
constexpr int kSquare = 120;

uint32_t checker_color(uint32_t x, uint32_t y) {
    return ((x / 64 + y / 64) % 2) ? 0xFF282828 : 0xFF202020;
}
} // namespace

DummySource::DummySource(uint32_t width, uint32_t height)
    : width_(width), height_(height), canvas_(width * height * 4) {
    paint_background({0, 0, width_, height_});
}

void DummySource::paint_background(Rect r) {
    for (uint32_t y = r.y; y < r.y + r.h; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(canvas_.data() + size_t{y} * width_ * 4);
        for (uint32_t x = r.x; x < r.x + r.w; ++x) row[x] = checker_color(x, y);
    }
}

void DummySource::paint_square(Rect r, uint32_t bgra) {
    for (uint32_t y = r.y; y < r.y + r.h; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(canvas_.data() + size_t{y} * width_ * 4);
        for (uint32_t x = r.x; x < r.x + r.w; ++x) row[x] = bgra;
    }
}

bool DummySource::next_frame(Frame& out) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));

    Rect old{static_cast<uint32_t>(x_), static_cast<uint32_t>(y_), kSquare, kSquare};
    paint_background(old);

    x_ += dx_;
    y_ += dy_;
    if (x_ < 0 || x_ + kSquare > static_cast<int>(width_)) {
        dx_ = -dx_;
        x_ += 2 * dx_;
    }
    if (y_ < 0 || y_ + kSquare > static_cast<int>(height_)) {
        dy_ = -dy_;
        y_ += 2 * dy_;
    }

    ++frame_id_;
    uint32_t hue = frame_id_ * 2;
    uint32_t color = 0xFF000000 | ((hue & 0xFF) << 16) | (((hue * 3) & 0xFF) << 8) | ((255 - (hue & 0xFF)));
    Rect neu{static_cast<uint32_t>(x_), static_cast<uint32_t>(y_), kSquare, kSquare};
    paint_square(neu, color);

    out.width = width_;
    out.height = height_;
    out.pixels = canvas_;
    out.rects = {old, neu};
    out.id = frame_id_;
    return true;
}

} // namespace krg
