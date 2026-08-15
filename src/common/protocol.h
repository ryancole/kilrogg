#pragma once
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

} // namespace krg
