#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include "common/frame.h"

namespace krg {

struct RectUpdate {
    Rect rect;
    std::vector<uint8_t> pixels; // rect.w * rect.h * 4, tightly packed
};

// Win32 window + D3D11 flip-model swapchain, vsync off (tearing allowed when
// supported). The GPU texture is the persistent remote framebuffer; apply()
// patches dirty rects into it and present() blits it to the window.
// All methods must be called from the thread that created the Presenter.
class Presenter {
public:
    struct Options {
        // The remote framebuffer is NV12 (video stream) rather than BGRA dirty
        // rects; copy_video_frame()/present() replace apply()/present().
        bool video_mode = false;
        // Pace on the swapchain's frame-latency waitable object at one frame
        // of latency and present with sync interval 1, instead of presenting
        // the instant a frame arrives with tearing allowed. Costs up to a
        // refresh of latency and buys a picture that does not judder when the
        // capture rate and the local refresh rate disagree.
        bool waitable = false;
        // Draw the latency/throughput overlay.
        bool stats = false;
    };

    static std::unique_ptr<Presenter> create(uint32_t frame_width, uint32_t frame_height,
                                             const Options& options = {});
    ~Presenter();

    // Drains the message queue; returns false once the window is closed.
    bool pump();

    // Repoints the presenter at a differently sized remote framebuffer, for a
    // reconnect to a sender whose display mode changed. The window keeps its
    // own size and position — the frame is stretched to the client rect either
    // way. Must not race copy_video_frame(), i.e. call it with no decoder
    // attached. False leaves the presenter without a framebuffer to draw.
    bool set_frame_size(uint32_t frame_width, uint32_t frame_height);

    void apply(const std::vector<RectUpdate>& updates);
    // Copies a decoded NV12 frame (array slice `subresource`) into the
    // presenter's own texture; callable from the decode thread.
    void copy_video_frame(ID3D11Texture2D* nv12, UINT subresource);
    void present();

    // Remote cursor overlay, drawn on top of the frame by present(). Callable
    // from the receive thread. (x, y) is the shape's top-left in frame
    // coordinates; `bgra` is w*h*4 straight-alpha color and `invert` is a
    // w*h mask (255 = invert the frame pixel underneath).
    void set_cursor_pos(int32_t x, int32_t y, bool visible);
    void set_cursor_shape(uint32_t w, uint32_t h, const uint8_t* bgra, const uint8_t* invert);

    // Replaces the overlay text (newline-separated); null or empty removes it.
    // Carries the latency figures under --stats, and the connection's state
    // when there is no connection to take figures from. Same thread as
    // present().
    void set_overlay_text(const char* text);

    // Sender clock is irrelevant here: this is the local clock right after the
    // most recent Present call returned.
    int64_t last_present_us() const { return last_present_us_; }

    // Milliseconds from a Present call to the vblank that actually scanned it
    // out, or a negative value when DXGI has no statistics to pair up — which
    // is the normal answer for a torn present.
    double scanout_delay_ms();

    Microsoft::WRL::ComPtr<ID3D11Device> device() const { return device_; }

private:
    Presenter() = default;
    bool init(uint32_t frame_width, uint32_t frame_height);
    bool create_frame_textures();
    bool init_overlay();
    void handle_resize(uint32_t w, uint32_t h);
    void draw_overlay(const RECT& client);
    void drain_debug_messages();
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    uint32_t frame_w_ = 0, frame_h_ = 0;
    bool tearing_ = false;
    bool video_mode_ = false;
    bool waitable_ = false;
    bool stats_ = false;   // draw the latency figures; --stats
    bool overlay_ = false; // the text machinery works at all, figures or not
    UINT swap_flags_ = 0;
    HANDLE frame_latency_waitable_ = nullptr;
    bool present_error_logged_ = false;
    Microsoft::WRL::ComPtr<ID3D11InfoQueue> info_queue_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frame_tex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> frame_srv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_nv12_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_tex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nv12_y_srv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nv12_uv_srv_;

    // Cursor overlay state; written by the receive thread, drawn by present().
    std::mutex cursor_mutex_;
    int32_t cursor_x_ = 0, cursor_y_ = 0;
    bool cursor_visible_ = false;
    uint32_t cursor_w_ = 0, cursor_h_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cursor_tex_, cursor_inv_tex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursor_srv_, cursor_inv_srv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_cursor_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_cursor_, ps_cursor_nv12_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cursor_cb_;

    // Stats overlay. GDI rasterises the text into a DIB that is uploaded as a
    // texture and drawn as one alpha-blended quad — a few hundred glyphs a
    // second is not worth a glyph atlas or a Direct2D device of its own.
    HDC overlay_dc_ = nullptr;
    HBITMAP overlay_bmp_ = nullptr;
    HGDIOBJ overlay_old_bmp_ = nullptr;
    HFONT overlay_font_ = nullptr;
    uint8_t* overlay_bits_ = nullptr;
    uint32_t overlay_tex_w_ = 0, overlay_tex_h_ = 0;
    uint32_t overlay_used_w_ = 0, overlay_used_h_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> overlay_tex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> overlay_srv_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_overlay_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> overlay_blend_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> point_sampler_;

    // Present-to-scanout pairing: DXGI reports the id of the frame that made it
    // to the glass, which has to be matched against when we submitted it.
    struct PresentRecord {
        UINT count = 0;
        int64_t us = 0;
    };
    PresentRecord present_log_[16];
    size_t present_log_next_ = 0;
    int64_t last_present_us_ = 0;
    double last_scanout_ms_ = -1;
};

} // namespace krg
