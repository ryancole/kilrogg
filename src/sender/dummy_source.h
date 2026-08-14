#pragma once
#include "sender/frame_source.h"

namespace krg {

// Synthetic frame source: a bouncing square over a checkerboard, produced at
// ~60fps with proper dirty rects. Lets the whole pipeline run end-to-end
// before (or without) real screen capture.
class DummySource final : public FrameSource {
public:
    DummySource(uint32_t width, uint32_t height);

    bool next_frame(Frame& out) override;
    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }

private:
    void paint_background(Rect r);
    void paint_square(Rect r, uint32_t bgra);

    uint32_t width_, height_;
    std::vector<uint8_t> canvas_;
    int x_ = 40, y_ = 40, dx_ = 7, dy_ = 5;
    uint32_t frame_id_ = 0;
};

} // namespace krg
