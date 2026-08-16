#pragma once
#include <cstddef>
#include <cstdint>

namespace krg {

constexpr uint32_t kMagic = 0x3147524B;      // "KRG1": LZ4 dirty-rect stream
constexpr uint32_t kMagicVideo = 0x3347524B; // "KRG3": compressed video stream
constexpr uint32_t kMagicClient = 0x4347524B; // "KRGC": receiver's opening hello
constexpr uint16_t kDefaultPort = 47800;

// Video codecs, as a bitmask in ClientHello::codecs and a single value in
// Hello::codec. The receiver advertises what it can decode and the sender
// picks, so an HEVC-capable sender still works with an H.264-only receiver.
constexpr uint32_t kCodecNone = 0; // KRG1 (LZ4) streams
constexpr uint32_t kCodecH264 = 1;
constexpr uint32_t kCodecHevc = 2;

// Wire format (little-endian, Windows-only):
//   on connect:  ClientHello (receiver -> sender), then Hello (sender -> receiver)
//   forward:     KRG1: FrameHeader, then rect_count x (RectHeader + comp_size bytes)
//                KRG3: VideoPacketHeader, then `size` payload bytes
//   back:        ControlHeader, then `size` payload bytes (receiver -> sender)
// Pixels are BGRA8. A full-frame rect acts as the KRG1 keyframe.
#pragma pack(push, 1)
struct ClientHello {
    uint32_t magic;
    uint32_t codecs; // kCodecH264 | kCodecHevc, whatever this receiver can decode
    uint32_t flags;  // reserved, must be 0
};

struct Hello {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t codec; // the one codec this stream uses; kCodecNone for KRG1
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

// KRG3: after Hello, the stream is a sequence of these followed by `size`
// payload bytes: Annex B video data (parameter sets arrive in-band before each
// IDR), a CursorUpdate when kPacketCursor is set, or a Pong when kPacketPong is.
//
// The timestamps are the sender's half of the end-to-end latency budget:
// `capture_us` is on the sender's clock (see now_us) and `encode_us` is the
// elapsed capture-to-sink time, which is free of clock skew and so is usable
// as-is. Both are zero on non-video packets.
struct VideoPacketHeader {
    uint32_t size;
    uint32_t flags; // kPacketKeyframe | kPacketCursor | kPacketPong
    int64_t capture_us;
    uint32_t encode_us;
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

// Reply to kControlPing, echoed so the receiver can pair it with its send and
// derive the round trip. `server_us` is the sender's clock when it replied.
struct Pong {
    int64_t client_us;
    int64_t server_us;
};

// Back-channel. The receiver is the only one that sends these.
struct ControlHeader {
    uint32_t type;
    uint32_t size; // payload bytes following this header
};
#pragma pack(pop)

constexpr uint32_t kPacketKeyframe = 1;
constexpr uint32_t kPacketCursor = 2;
constexpr uint32_t kPacketPong = 4;

// The receiver lost decoder sync (or has yet to gain it) and needs an IDR.
// With a long GOP this is the only thing that produces one.
constexpr uint32_t kControlKeyframe = 1;
// Payload is an int64_t: the receiver's clock at send. Answered with a Pong.
constexpr uint32_t kControlPing = 2;

constexpr uint32_t kMaxControlPayload = 64;

// Nothing on the wire is authenticated, so every header a peer sends is
// arithmetic waiting to go wrong on the other side: a rect is turned into a
// D3D11_BOX and a cursor size into a texture allocation. These are the checks
// that stand between the two, kept here beside the structs they validate so
// the rules travel with the format rather than with one reader of it.

// The largest LZ4 payload one rect may claim. A rect covering a 4K screen
// compresses to a few megabytes at worst, so this is loose enough never to
// refuse real data and tight enough that a bogus length cannot ask for a
// gigabyte buffer before anything looks at it.
constexpr uint32_t kMaxCompressedRect = 64u << 20;

// Windows cursors are at most 256x256 in practice; this leaves room and still
// bounds the texture a shape update can ask the presenter to create.
constexpr uint32_t kMaxCursorDimension = 1024;

// A KRG1 rect that can be applied to a `width` x `height` frame.
//
// The bounds are written as subtractions rather than as `x + w <= width`
// because the sum is uint32 arithmetic and wraps: x = 0xFFFFF000 with w = 0x1000
// sums to 0, which passes any comparison against a width. Subtracting from the
// bound cannot wrap, since x has already been shown not to exceed it.
inline bool valid_rect_header(const RectHeader& rh, uint32_t width, uint32_t height) {
    if (rh.w == 0 || rh.h == 0) return false;
    if (rh.x > width || rh.w > width - rh.x) return false;
    if (rh.y > height || rh.h > height - rh.y) return false;
    // Widened for the same reason. A rect whose true size does not fit in the
    // uint32 the sender declared it in can never match, which is the answer.
    if (rh.raw_size != uint64_t{rh.w} * rh.h * 4) return false;
    return rh.comp_size > 0 && rh.comp_size <= kMaxCompressedRect;
}

// A cursor packet whose declared shape matches the bytes that arrived with it:
// `payload_size` counts the CursorUpdate itself plus everything after it.
// Without a shape the dimensions describe nothing and are not read, so they are
// not judged either — only the length has to agree.
inline bool valid_cursor_payload(const CursorUpdate& cu, size_t payload_size) {
    if (cu.has_shape) {
        if (cu.width == 0 || cu.height == 0) return false;
        if (cu.width > kMaxCursorDimension || cu.height > kMaxCursorDimension) return false;
    }
    const uint64_t pixels = uint64_t{cu.width} * cu.height;
    // BGRA plus the one-byte invert mask: five bytes a pixel.
    const uint64_t expected = sizeof(CursorUpdate) + (cu.has_shape ? pixels * 5 : 0);
    return payload_size == expected;
}

} // namespace krg
