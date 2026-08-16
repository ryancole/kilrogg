#include "sender/cursor_shape.h"

#include <cstring>

namespace krg {

bool convert_cursor_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si, const uint8_t* data,
                          CursorShape& out) {
    switch (si.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
        out.width = si.Width;
        out.height = si.Height;
        out.bgra.resize(size_t{out.width} * out.height * 4);
        out.invert.assign(size_t{out.width} * out.height, 0);
        for (uint32_t y = 0; y < out.height; ++y) {
            std::memcpy(out.bgra.data() + size_t{y} * out.width * 4,
                        data + size_t{y} * si.Pitch, size_t{out.width} * 4);
        }
        return true;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
        // 1bpp MSB-first AND mask stacked on top of a 1bpp XOR mask.
        // AND=0 draws the XOR bit as black/white; AND=1 + XOR=1 inverts the
        // screen (the I-beam is made entirely of these); AND=1 + XOR=0 is
        // transparent.
        out.width = si.Width;
        out.height = si.Height / 2;
        out.bgra.assign(size_t{out.width} * out.height * 4, 0);
        out.invert.assign(size_t{out.width} * out.height, 0);
        for (uint32_t y = 0; y < out.height; ++y) {
            const uint8_t* and_row = data + size_t{y} * si.Pitch;
            const uint8_t* xor_row = data + size_t{y + out.height} * si.Pitch;
            for (uint32_t x = 0; x < out.width; ++x) {
                bool and_bit = (and_row[x / 8] >> (7 - x % 8)) & 1;
                bool xor_bit = (xor_row[x / 8] >> (7 - x % 8)) & 1;
                size_t i = size_t{y} * out.width + x;
                if (!and_bit) {
                    out.bgra[i * 4 + 0] = out.bgra[i * 4 + 1] = out.bgra[i * 4 + 2] =
                        xor_bit ? 255 : 0;
                    out.bgra[i * 4 + 3] = 255;
                } else if (xor_bit) {
                    out.invert[i] = 255;
                }
            }
        }
        return true;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
        // 32bpp; alpha byte 0xFF means XOR the color with the screen (black
        // XOR = transparent, anything else approximated as invert), else the
        // color is opaque.
        out.width = si.Width;
        out.height = si.Height;
        out.bgra.assign(size_t{out.width} * out.height * 4, 0);
        out.invert.assign(size_t{out.width} * out.height, 0);
        for (uint32_t y = 0; y < out.height; ++y) {
            const uint8_t* row = data + size_t{y} * si.Pitch;
            for (uint32_t x = 0; x < out.width; ++x) {
                const uint8_t* src = row + size_t{x} * 4;
                size_t i = size_t{y} * out.width + x;
                if (src[3] == 0xFF) {
                    if (src[0] || src[1] || src[2]) out.invert[i] = 255;
                } else {
                    out.bgra[i * 4 + 0] = src[0];
                    out.bgra[i * 4 + 1] = src[1];
                    out.bgra[i * 4 + 2] = src[2];
                    out.bgra[i * 4 + 3] = 255;
                }
            }
        }
        return true;
    }
    }
    return false;
}

} // namespace krg
