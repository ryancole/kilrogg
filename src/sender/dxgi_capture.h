#pragma once
#include <memory>
#include <mutex>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "sender/frame_source.h"

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

// Desktop Duplication capture of the primary output. next_frame() returning
// false is the common case on a static screen (AcquireNextFrame times out
// rather than erroring) — callers just loop.
class DxgiCapture final : public FrameSource {
public:
    static std::unique_ptr<DxgiCapture> create(); // null on failure

    bool next_frame(Frame& out) override;

    // Zero-CPU-copy path for the video pipeline: the acquired desktop image
    // is copied GPU-side into a small texture pool. Same false-on-timeout
    // semantics as next_frame(). The returned texture stays valid until the
    // pool wraps (4 frames).
    bool next_frame_texture(Microsoft::WRL::ComPtr<ID3D11Texture2D>& out);

    Microsoft::WRL::ComPtr<ID3D11Device> device() const { return device_; }
    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }

    // Cursor state updated by the capture thread; safe to poll from another
    // thread. Copies the state and returns true if it changed since
    // `last_version` (start a fresh consumer at 0), advancing last_version.
    bool poll_cursor_pos(uint64_t& last_version, CursorPos& out);
    bool poll_cursor_shape(uint64_t& last_version, CursorShape& out);

private:
    DxgiCapture() = default;
    bool init();
    bool reinit_duplication();
    void update_cursor(const DXGI_OUTDUPL_FRAME_INFO& info);
    void collect_rects(const DXGI_OUTDUPL_FRAME_INFO& info, std::vector<Rect>& rects);
    // Shared acquire logic; on success `acquired` holds the desktop texture
    // and the caller must ReleaseFrame() after copying from it.
    bool acquire(Microsoft::WRL::ComPtr<ID3D11Texture2D>& acquired, bool& have_rects,
                 std::vector<Rect>& rects);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pool_[4];
    size_t pool_index_ = 0;
    std::vector<uint8_t> metadata_;
    std::vector<uint8_t> shape_buf_; // capture thread only
    uint32_t width_ = 0, height_ = 0;
    uint32_t frame_id_ = 0;
    bool first_frame_ = true;

    std::mutex cursor_mutex_;
    CursorPos cursor_pos_;
    CursorShape cursor_shape_;
    uint64_t cursor_pos_version_ = 0;
    uint64_t cursor_shape_version_ = 0;
};

} // namespace krg
