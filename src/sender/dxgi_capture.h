#pragma once
#include <atomic>
#include <memory>
#include <mutex>

#include <d3d11.h>
#include <dxgi1_5.h>
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
    uint32_t width() const override { return width_.load(std::memory_order_acquire); }
    uint32_t height() const override { return height_.load(std::memory_order_acquire); }
    // The format frames actually come out in. BGRA8 in every case the sender
    // can encode, but read from the captured texture rather than assumed: an
    // HDR desktop composites as scRGB half-float, and while DXGI is asked to
    // convert that away (see reinit_duplication), a machine where it does not
    // has to say so rather than stream a texture nothing ever wrote to.
    DXGI_FORMAT format() const {
        return static_cast<DXGI_FORMAT>(format_.load(std::memory_order_acquire));
    }
    // The display's refresh rate in whole Hz, i.e. the fastest rate frames can
    // arrive at. The encoder budgets bits per frame from the rate it is told,
    // so this is what it has to be told, and what submissions are paced to.
    uint32_t refresh_hz() const { return refresh_hz_.load(std::memory_order_acquire); }

    // Drops the duplication so nothing is being captured or composited on our
    // behalf while no client is attached; the next frame request brings it
    // back. Capture-thread only, since it races the acquire loop otherwise.
    void release_duplication();

    // True once, after the display mode has changed under us. Dimensions on
    // the wire are fixed for the life of a connection, so the caller's answer
    // is to drop the client and let it reconnect against the new width() and
    // height() — which are already updated by the time this returns true.
    bool take_mode_change() { return mode_changed_.exchange(false); }

    // Cursor state updated by the capture thread; safe to poll from another
    // thread. Copies the state and returns true if it changed since
    // `last_version` (start a fresh consumer at 0), advancing last_version.
    bool poll_cursor_pos(uint64_t& last_version, CursorPos& out);
    bool poll_cursor_shape(uint64_t& last_version, CursorShape& out);

private:
    DxgiCapture() = default;
    bool init();
    bool reinit_duplication();
    // Adopts a new display mode: publishes the dimensions, refresh rate and
    // pixel format, and throws away the textures cut to the old ones, which are
    // recreated lazily to match.
    void adopt_mode(uint32_t new_width, uint32_t new_height, uint32_t new_hz,
                    DXGI_FORMAT new_format);
    void update_cursor(const DXGI_OUTDUPL_FRAME_INFO& info);
    void collect_rects(const DXGI_OUTDUPL_FRAME_INFO& info, std::vector<Rect>& rects);
    // Shared acquire logic; on success `acquired` holds the desktop texture
    // and the caller must ReleaseFrame() after copying from it.
    bool acquire(Microsoft::WRL::ComPtr<ID3D11Texture2D>& acquired, bool& have_rects,
                 std::vector<Rect>& rects);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    // The interface that can be asked for a format; absent before Windows 10
    // 1703, where the desktop cannot have been HDR anyway.
    Microsoft::WRL::ComPtr<IDXGIOutput5> output5_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pool_[4];
    size_t pool_index_ = 0;
    std::vector<uint8_t> metadata_;
    std::vector<uint8_t> shape_buf_; // capture thread only
    // Written by the capture thread on a mode change, read by the run loop
    // when it sets a connection's dimensions.
    std::atomic<uint32_t> width_{0}, height_{0};
    std::atomic<uint32_t> refresh_hz_{60};
    std::atomic<uint32_t> format_{DXGI_FORMAT_B8G8R8A8_UNORM};
    std::atomic<bool> mode_changed_{false};
    uint32_t frame_id_ = 0;
    bool first_frame_ = true;
    bool lz4_format_warned_ = false; // capture thread only

    std::mutex cursor_mutex_;
    CursorPos cursor_pos_;
    CursorShape cursor_shape_;
    uint64_t cursor_pos_version_ = 0;
    uint64_t cursor_shape_version_ = 0;
};

} // namespace krg
