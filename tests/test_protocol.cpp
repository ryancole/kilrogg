#include <doctest/doctest.h>

#include "common/frame.h"
#include "common/protocol.h"

using namespace krg;

namespace {

constexpr uint32_t kWidth = 3440;
constexpr uint32_t kHeight = 1440;

RectHeader rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return RectHeader{x, y, w, h, 64, w * h * 4};
}

CursorUpdate cursor(uint32_t w, uint32_t h, bool has_shape) {
    CursorUpdate cu{};
    cu.x = 100;
    cu.y = 200;
    cu.visible = 1;
    cu.has_shape = has_shape ? 1 : 0;
    cu.width = w;
    cu.height = h;
    return cu;
}

} // namespace

TEST_CASE("a rect inside the frame is accepted") {
    CHECK(valid_rect_header(rect(0, 0, kWidth, kHeight), kWidth, kHeight));
    CHECK(valid_rect_header(rect(100, 100, 32, 32), kWidth, kHeight));
    // Flush against the far edge, which is legal and easy to lose to an
    // off-by-one.
    CHECK(valid_rect_header(rect(kWidth - 1, kHeight - 1, 1, 1), kWidth, kHeight));
}

TEST_CASE("a rect hanging over the edge is refused") {
    CHECK_FALSE(valid_rect_header(rect(kWidth, 0, 1, 1), kWidth, kHeight));
    CHECK_FALSE(valid_rect_header(rect(0, kHeight, 1, 1), kWidth, kHeight));
    CHECK_FALSE(valid_rect_header(rect(kWidth - 10, 0, 11, 1), kWidth, kHeight));
    CHECK_FALSE(valid_rect_header(rect(0, kHeight - 10, 1, 11), kWidth, kHeight));
}

TEST_CASE("a rect whose origin and size sum past 2^32 is refused") {
    // The bound written as `x + w <= width` wraps here and lets the rect
    // through, which then reaches the presenter as an inverted D3D11_BOX. This
    // is the case the subtraction form exists for.
    CHECK_FALSE(valid_rect_header(rect(0xFFFFF000, 0, 0x1000, 1), kWidth, kHeight));
    CHECK_FALSE(valid_rect_header(rect(0, 0xFFFFF000, 1, 0x1000), kWidth, kHeight));
    CHECK_FALSE(valid_rect_header(rect(0xFFFFFFFF, 0, 1, 1), kWidth, kHeight));
}

TEST_CASE("an empty rect is refused") {
    CHECK_FALSE(valid_rect_header(rect(0, 0, 0, 10), kWidth, kHeight));
    CHECK_FALSE(valid_rect_header(rect(0, 0, 10, 0), kWidth, kHeight));
}

TEST_CASE("the declared raw size has to be the size the rect actually is") {
    RectHeader rh = rect(0, 0, 32, 32);
    CHECK(valid_rect_header(rh, kWidth, kHeight));
    rh.raw_size += 4;
    CHECK_FALSE(valid_rect_header(rh, kWidth, kHeight));
    rh.raw_size = 0;
    CHECK_FALSE(valid_rect_header(rh, kWidth, kHeight));
}

TEST_CASE("a rect whose true size will not fit in the declared raw size is refused") {
    // w * h * 4 overflows uint32 here, so no raw_size can be right — including
    // the 0 that the wrapped multiply produces.
    RectHeader rh{0, 0, 0x10000, 0x10000, 64, 0};
    CHECK_FALSE(valid_rect_header(rh, 0xFFFFFFFF, 0xFFFFFFFF));
    // The truncated product is 0, so a wrapping check would have agreed with
    // the 0 above. Nothing else matches either.
    rh.raw_size = 0xFFFFFFFF;
    CHECK_FALSE(valid_rect_header(rh, 0xFFFFFFFF, 0xFFFFFFFF));
}

TEST_CASE("a compressed payload has to be present and bounded") {
    RectHeader rh = rect(0, 0, 32, 32);
    rh.comp_size = 0;
    CHECK_FALSE(valid_rect_header(rh, kWidth, kHeight));
    rh.comp_size = kMaxCompressedRect;
    CHECK(valid_rect_header(rh, kWidth, kHeight));
    rh.comp_size = kMaxCompressedRect + 1;
    CHECK_FALSE(valid_rect_header(rh, kWidth, kHeight));
}

TEST_CASE("a cursor packet with no shape is just the header") {
    CHECK(valid_cursor_payload(cursor(0, 0, false), sizeof(CursorUpdate)));
    CHECK_FALSE(valid_cursor_payload(cursor(0, 0, false), sizeof(CursorUpdate) + 1));
    CHECK_FALSE(valid_cursor_payload(cursor(0, 0, false), sizeof(CursorUpdate) - 1));
    // Dimensions describe nothing without a shape, so they are not read and
    // are not judged.
    CHECK(valid_cursor_payload(cursor(9999, 9999, false), sizeof(CursorUpdate)));
}

TEST_CASE("a cursor shape is five bytes a pixel: BGRA plus the invert mask") {
    const size_t pixels = 32 * 32;
    CHECK(valid_cursor_payload(cursor(32, 32, true), sizeof(CursorUpdate) + pixels * 5));
    CHECK_FALSE(valid_cursor_payload(cursor(32, 32, true), sizeof(CursorUpdate) + pixels * 4));
    CHECK_FALSE(valid_cursor_payload(cursor(32, 32, true), sizeof(CursorUpdate) + pixels * 5 - 1));
}

TEST_CASE("a cursor shape has to have an area, and a bounded one") {
    const size_t pixels = size_t{kMaxCursorDimension} * kMaxCursorDimension;
    CHECK(valid_cursor_payload(cursor(kMaxCursorDimension, kMaxCursorDimension, true),
                               sizeof(CursorUpdate) + pixels * 5));
    // One past the bound, with a payload that agrees with it — the length check
    // alone would pass this, and the presenter would be asked to make the
    // texture.
    const size_t over = size_t{kMaxCursorDimension + 1} * kMaxCursorDimension;
    CHECK_FALSE(valid_cursor_payload(cursor(kMaxCursorDimension + 1, kMaxCursorDimension, true),
                                     sizeof(CursorUpdate) + over * 5));
    CHECK_FALSE(valid_cursor_payload(cursor(0, 32, true), sizeof(CursorUpdate)));
    CHECK_FALSE(valid_cursor_payload(cursor(32, 0, true), sizeof(CursorUpdate)));
}

TEST_CASE("the dimension bound is what stops an area big enough to overflow") {
    // 0x10000 square is 2^32 pixels. The bound refuses it on the dimensions
    // before the payload arithmetic ever runs, which is the order that matters:
    // the multiply is what would wrap.
    CursorUpdate cu = cursor(0x10000, 0x10000, true);
    CHECK_FALSE(valid_cursor_payload(cu, sizeof(CursorUpdate)));
    CHECK_FALSE(valid_cursor_payload(cu, sizeof(CursorUpdate) + 5));
}

TEST_CASE("a rect inside the frame is left alone by the clamp") {
    const Rect r = clamp_rect({10, 20, 30, 40}, 100, 100);
    CHECK(r.x == 10);
    CHECK(r.y == 20);
    CHECK(r.w == 30);
    CHECK(r.h == 40);
}

TEST_CASE("a rect hanging over the edge is trimmed to it") {
    // DXGI reports move rects whose destination straddles the edge of the
    // display; reading one whole would run off the end of the pixel buffer.
    const Rect wide = clamp_rect({90, 0, 30, 10}, 100, 100);
    CHECK(wide.x == 90);
    CHECK(wide.w == 10);
    const Rect tall = clamp_rect({0, 95, 10, 30}, 100, 100);
    CHECK(tall.y == 95);
    CHECK(tall.h == 5);
}

TEST_CASE("a rect starting outside the frame collapses to nothing") {
    // Not clamped inward: there is no part of it the frame contains, so the
    // caller drops it rather than sending a rect from somewhere else.
    const Rect past = clamp_rect({100, 0, 10, 10}, 100, 100);
    CHECK(past.w == 0);
    CHECK(past.h == 0);
    const Rect below = clamp_rect({0, 100, 10, 10}, 100, 100);
    CHECK(below.w == 0);
    CHECK(below.h == 0);
}
