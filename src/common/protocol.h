#pragma once
#include <cstdint>

namespace krg {

constexpr uint32_t kMagic = 0x3147524B;      // "KRG1": LZ4 dirty-rect stream
constexpr uint32_t kMagicVideo = 0x3247524B; // "KRG2": H.264 video stream
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

// KRG2: after Hello, the stream is a sequence of these followed by `size`
// payload bytes: H.264 Annex B data (SPS/PPS arrive in-band before each IDR),
// or a CursorUpdate when kPacketCursor is set.
struct VideoPacketHeader {
    uint32_t size;
    uint32_t flags; // kPacketKeyframe | kPacketCursor
};

// Cursor overlay state; the desktop image itself never contains the hardware
// cursor. (x, y) is where the shape's top-left goes, in desktop coordinates
// (DXGI reports the draw origin directly, so no hotspot math on either side).
// When has_shape is set, width*height*4 bytes of straight-alpha BGRA followed
// by width*height invert-mask bytes (255 = invert the pixel underneath, for
// monochrome/masked XOR cursors) complete the packet.
struct CursorUpdate {
    int32_t x, y;
    uint8_t visible;
    uint8_t has_shape;
    uint16_t reserved;
    uint32_t width, height;
};
#pragma pack(pop)

constexpr uint32_t kPacketKeyframe = 1;
constexpr uint32_t kPacketCursor = 2;

} // namespace krg
