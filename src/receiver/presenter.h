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
    // video_mode: the remote framebuffer is NV12 (H.264 stream) rather than
    // BGRA dirty rects; copy_video_frame()/present() replace apply()/present().
    static std::unique_ptr<Presenter> create(uint32_t frame_width, uint32_t frame_height,
                                             bool video_mode = false);

    // Drains the message queue; returns false once the window is closed.
    bool pump();

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

    Microsoft::WRL::ComPtr<ID3D11Device> device() const { return device_; }

private:
    Presenter() = default;
    bool init(uint32_t frame_width, uint32_t frame_height);
    void handle_resize(uint32_t w, uint32_t h);
    void drain_debug_messages();
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    uint32_t frame_w_ = 0, frame_h_ = 0;
    bool tearing_ = false;
    bool video_mode_ = false;
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
};

} // namespace krg
