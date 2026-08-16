#include <doctest/doctest.h>

#include <ostream>
#include <vector>

#include "sender/cursor_shape.h"

using namespace krg;

namespace {

DXGI_OUTDUPL_POINTER_SHAPE_INFO info(UINT type, UINT width, UINT height, UINT pitch) {
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    si.Type = type;
    si.Width = width;
    si.Height = height;
    si.Pitch = pitch;
    return si;
}

// One pixel out of the flattened BGRA, so failures read as a colour rather than
// as four subscript expressions.
struct Bgra {
    uint8_t b, g, r, a;
    bool operator==(const Bgra&) const = default;
};

// So a mismatch reports the two colours rather than doctest's "{?}".
std::ostream& operator<<(std::ostream& os, const Bgra& p) {
    return os << "bgra(" << int(p.b) << ',' << int(p.g) << ',' << int(p.r) << ',' << int(p.a)
              << ')';
}

Bgra pixel(const CursorShape& s, uint32_t x, uint32_t y) {
    const size_t i = (size_t{y} * s.width + x) * 4;
    return {s.bgra[i], s.bgra[i + 1], s.bgra[i + 2], s.bgra[i + 3]};
}

uint8_t invert_at(const CursorShape& s, uint32_t x, uint32_t y) {
    return s.invert[size_t{y} * s.width + x];
}

} // namespace

TEST_CASE("a colour cursor is repacked out of its padded rows") {
    // DXGI hands back rows padded to its own pitch; everything downstream — the
    // wire format and the receiver's texture upload — assumes tight packing.
    const auto si = info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 2, 2, 12);
    const std::vector<uint8_t> data{
        1, 2, 3, 4, 5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE,          // row 0 + padding
        9, 10, 11, 12, 13, 14, 15, 16, 0xEE, 0xEE, 0xEE, 0xEE,   // row 1 + padding
    };

    CursorShape out;
    REQUIRE(convert_cursor_shape(si, data.data(), out));
    CHECK(out.width == 2);
    CHECK(out.height == 2);
    REQUIRE(out.bgra.size() == 16);
    CHECK(out.bgra == std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    // A colour cursor carries its own alpha; nothing about it inverts.
    CHECK(out.invert == std::vector<uint8_t>(4, 0));
}

TEST_CASE("a monochrome cursor's AND and XOR masks become colour plus invert") {
    // Two 1bpp planes stacked: AND on top, XOR below, so a 2x2 cursor arrives
    // as four rows. Bits are MSB-first, so x=0 is 0x80 and x=1 is 0x40.
    //
    //   AND=0, XOR=0  ->  opaque black
    //   AND=0, XOR=1  ->  opaque white
    //   AND=1, XOR=1  ->  invert whatever is underneath (the I-beam)
    //   AND=1, XOR=0  ->  transparent
    const auto si = info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 2, 4, 4);
    const std::vector<uint8_t> data{
        0x00, 0, 0, 0, // AND row 0: both 0
        0xC0, 0, 0, 0, // AND row 1: both 1
        0x40, 0, 0, 0, // XOR row 0: x=0 clear, x=1 set
        0x80, 0, 0, 0, // XOR row 1: x=0 set, x=1 clear
    };

    CursorShape out;
    REQUIRE(convert_cursor_shape(si, data.data(), out));
    CHECK(out.width == 2);
    CHECK(out.height == 2); // half of what arrived: the other half was the mask

    CHECK(pixel(out, 0, 0) == Bgra{0, 0, 0, 255});       // opaque black
    CHECK(pixel(out, 1, 0) == Bgra{255, 255, 255, 255}); // opaque white
    CHECK(pixel(out, 0, 1) == Bgra{0, 0, 0, 0});         // inverting, so no colour
    CHECK(pixel(out, 1, 1) == Bgra{0, 0, 0, 0});         // transparent

    CHECK(invert_at(out, 0, 0) == 0);
    CHECK(invert_at(out, 1, 0) == 0);
    CHECK(invert_at(out, 0, 1) == 255);
    CHECK(invert_at(out, 1, 1) == 0);
}

TEST_CASE("a masked colour cursor reads its alpha byte as an XOR flag, not as alpha") {
    // 0xFF here does not mean opaque, it means XOR with the screen: black XOR
    // is transparent, anything else is approximated as an invert.
    const auto si = info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, 3, 1, 16);
    const std::vector<uint8_t> data{
        10, 20, 30, 0x00,    // not XOR: an opaque colour
        0, 0, 0, 0xFF,       // XOR with black: transparent
        255, 255, 255, 0xFF, // XOR with a colour: invert
        0xEE, 0xEE, 0xEE, 0xEE,
    };

    CursorShape out;
    REQUIRE(convert_cursor_shape(si, data.data(), out));
    CHECK(out.width == 3);
    CHECK(out.height == 1);

    CHECK(pixel(out, 0, 0) == Bgra{10, 20, 30, 255});
    CHECK(pixel(out, 1, 0) == Bgra{0, 0, 0, 0});
    CHECK(pixel(out, 2, 0) == Bgra{0, 0, 0, 0});

    CHECK(invert_at(out, 0, 0) == 0);
    CHECK(invert_at(out, 1, 0) == 0);
    CHECK(invert_at(out, 2, 0) == 255);
}

TEST_CASE("a shape type nothing here reads is refused rather than guessed at") {
    const auto si = info(99, 2, 2, 8);
    const std::vector<uint8_t> data(64, 0);
    CursorShape out;
    CHECK_FALSE(convert_cursor_shape(si, data.data(), out));
    // The receiver goes on drawing the cursor it already had.
    CHECK(out.width == 0);
    CHECK(out.bgra.empty());
}

TEST_CASE("every conversion leaves the two planes sized to the same pixel count") {
    // The wire format writes width*height*4 bytes of colour followed by
    // width*height mask bytes, and the receiver's length check is derived from
    // the same numbers — so a conversion that sized them differently would put
    // a packet on the wire that the far end refuses.
    const std::vector<uint8_t> data(4096, 0x81);
    for (UINT type : {DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR,
                      DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME,
                      DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR}) {
        CursorShape out;
        REQUIRE(convert_cursor_shape(info(type, 8, 16, 32), data.data(), out));
        CAPTURE(type);
        CHECK(out.bgra.size() == size_t{out.width} * out.height * 4);
        CHECK(out.invert.size() == size_t{out.width} * out.height);
    }
}
