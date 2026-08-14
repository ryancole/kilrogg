#pragma once
#include "common/frame.h"

namespace krg {

class FrameSource {
public:
    virtual ~FrameSource() = default;

    // Blocks until a new frame is available (or an internal timeout expires).
    // Returns false when there is no new frame — normal when the screen is
    // static, not an error. The caller should just loop.
    virtual bool next_frame(Frame& out) = 0;

    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
};

} // namespace krg
