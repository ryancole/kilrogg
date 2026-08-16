#pragma once
#include <cstdint>
#include <vector>

#include <dxgi1_2.h>

namespace krg {

// Hardware cursor state observed alongside captured frames. Shapes are
// normalized to straight-alpha BGRA plus an invert mask (255 = XOR-style
// cursor pixel that inverts whatever is underneath it).
struct CursorPos {
    int32_t x = 0, y = 0; // draw origin of the shape's top-left, desktop coords
    bool visible = false;
};

struct CursorShape {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;   // width * height * 4
    std::vector<uint8_t> invert; // width * height
};

// Flattens whichever of DXGI's three cursor encodings arrived into the one the
// wire format and the receiver's shader speak. `data` is the shape buffer
// GetFramePointerShape filled, described by `si`; false means a shape type
// nothing here knows how to read, which is not fatal — the receiver simply goes
// on drawing the cursor it already had.
//
// Split out from the capture loop because it is the one piece of that file with
// no GPU, no duplication and no clock in it: pure bit arithmetic over a buffer,
// which is exactly the kind of code that is wrong in one corner for years
// without anyone noticing on screen.
bool convert_cursor_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si, const uint8_t* data,
                          CursorShape& out);

} // namespace krg
