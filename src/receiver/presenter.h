#pragma once
#include <cstdint>
#include <memory>
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
    static std::unique_ptr<Presenter> create(uint32_t frame_width, uint32_t frame_height);

    // Drains the message queue; returns false once the window is closed.
    bool pump();

    void apply(const std::vector<RectUpdate>& updates);
    void present();

private:
    Presenter() = default;
    bool init(uint32_t frame_width, uint32_t frame_height);
    void handle_resize(uint32_t w, uint32_t h);
    void drain_debug_messages();
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    uint32_t frame_w_ = 0, frame_h_ = 0;
    bool tearing_ = false;
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
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};

} // namespace krg
