#include "receiver/presenter.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include <d3dcompiler.h>

#include "common/clock.h"
#include "common/log.h"

using Microsoft::WRL::ComPtr;

#define KRG_HRB(expr)                                         \
    do {                                                      \
        HRESULT hrb_ = (expr);                                \
        if (FAILED(hrb_)) {                                   \
            KRG_LOG("%s failed (hr=0x%08lX)", #expr, hrb_);   \
            return false;                                     \
        }                                                     \
    } while (0)

namespace krg {

namespace {

const char kShaderSrc[] = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps_main(VSOut i) : SV_Target { return tex.Sample(smp, i.uv); }

// NV12 (BT.709, limited range) to RGB.
Texture2D texY  : register(t0);
Texture2D texUV : register(t1);
float3 nv12_to_rgb(float2 uv_frame) {
    float y = (texY.Sample(smp, uv_frame).r - 16.0 / 255.0) * (255.0 / 219.0);
    float2 uv = (texUV.Sample(smp, uv_frame).rg - 0.5) * (255.0 / 224.0);
    float3 rgb = float3(
        y + 1.5748 * uv.y,
        y - 0.1873 * uv.x - 0.4681 * uv.y,
        y + 1.8556 * uv.x);
    return saturate(rgb);
}
float4 ps_nv12(VSOut i) : SV_Target { return float4(nv12_to_rgb(i.uv), 1); }

// Cursor overlay: a small quad whose pixel shader samples the frame texture
// itself as the background, so straight-alpha blending and XOR/invert cursor
// pixels both work without a blend state or backbuffer read.
cbuffer CursorCB : register(b0) {
    float4 cur_dst; // cursor rect in NDC: x0, y0 (top-left), x1, y1
    float4 cur_uv;  // same rect in frame UV space
};
struct CursorVSOut { float4 pos : SV_Position; float2 uvc : TEXCOORD0; float2 uvf : TEXCOORD1; };
CursorVSOut vs_cursor(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1); // strip: (0,0) (1,0) (0,1) (1,1)
    CursorVSOut o;
    o.pos = float4(lerp(cur_dst.xy, cur_dst.zw, t), 0, 1);
    o.uvc = t;
    o.uvf = lerp(cur_uv.xy, cur_uv.zw, t);
    return o;
}
Texture2D curColor  : register(t2);
Texture2D curInvert : register(t3);
float3 cursor_blend(float3 bg, float2 uvc) {
    float4 c = curColor.Sample(smp, uvc);
    float inv = curInvert.Sample(smp, uvc).r;
    return lerp(lerp(bg, c.rgb, c.a), 1 - bg, inv);
}
float4 ps_cursor(CursorVSOut i) : SV_Target {
    return float4(cursor_blend(tex.Sample(smp, i.uvf).rgb, i.uvc), 1);
}
float4 ps_cursor_nv12(CursorVSOut i) : SV_Target {
    return float4(cursor_blend(nv12_to_rgb(i.uvf), i.uvc), 1);
}

// Stats overlay: a straight-alpha BGRA texture drawn over everything through
// the blend state, reusing vs_cursor to place the quad.
Texture2D overlayTex : register(t4);
float4 ps_overlay(CursorVSOut i) : SV_Target { return overlayTex.Sample(smp, i.uvf); }
)";

constexpr char kWndClass[] = "KilroggWindow";

// Overlay geometry: a margin inside the text box, the box's own opacity over
// the video, and how far the box sits from the top-left corner of the window.
constexpr LONG kOverlayPad = 6;
constexpr int kOverlayBoxAlpha = 170;
constexpr float kOverlayMargin = 12.0f;

} // namespace

LRESULT CALLBACK Presenter::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lp);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<Presenter*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (self && wp != SIZE_MINIMIZED) self->handle_resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

std::unique_ptr<Presenter> Presenter::create(uint32_t frame_width, uint32_t frame_height,
                                             const Options& options) {
    std::unique_ptr<Presenter> p(new Presenter());
    p->video_mode_ = options.video_mode;
    p->waitable_ = options.waitable;
    p->stats_ = options.stats;
    if (!p->init(frame_width, frame_height)) return nullptr;
    return p;
}

Presenter::~Presenter() {
    if (overlay_dc_) {
        if (overlay_old_bmp_) SelectObject(overlay_dc_, overlay_old_bmp_);
        DeleteDC(overlay_dc_);
    }
    if (overlay_bmp_) DeleteObject(overlay_bmp_);
    if (overlay_font_) DeleteObject(overlay_font_);
    if (frame_latency_waitable_) CloseHandle(frame_latency_waitable_);
}

bool Presenter::init(uint32_t frame_width, uint32_t frame_height) {
    frame_w_ = frame_width;
    frame_h_ = frame_height;

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursorA(nullptr, reinterpret_cast<LPCSTR>(IDC_ARROW));
    wc.lpszClassName = kWndClass;
    RegisterClassExA(&wc);

    // Open at remote resolution, scaled down to fit the local work area.
    RECT wa{0, 0, 1280, 720};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    double scale = std::min({1.0,
                             (wa.right - wa.left) * 0.95 / frame_w_,
                             (wa.bottom - wa.top) * 0.95 / frame_h_});
    RECT rc{0, 0, static_cast<LONG>(frame_w_ * scale), static_cast<LONG>(frame_h_ * scale)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = CreateWindowExA(0, kWndClass, "kilrogg", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                            nullptr, wc.hInstance, this);
    if (!hwnd_) {
        KRG_LOG("window creation failed");
        return false;
    }

    // Set KILROGG_D3D_DEBUG=1 to run with the D3D11 debug layer; validation
    // messages are echoed to stderr after each present.
    // VIDEO_SUPPORT + BGRA: required for sharing the device with the MF/DXVA
    // decoder in video mode; harmless otherwise.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (GetEnvironmentVariableA("KILROGG_D3D_DEBUG", nullptr, 0) > 0) {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        KRG_LOG("debug layer unavailable, falling back to normal device");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                               D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    }
    if (FAILED(hr)) {
        KRG_LOG("D3D11CreateDevice failed (hr=0x%08lX)", hr);
        return false;
    }
    if (flags & D3D11_CREATE_DEVICE_DEBUG) {
        device_.As(&info_queue_);
        KRG_LOG("D3D11 debug layer enabled");
    }
    // The MF decoder's worker threads share this device in video mode, and
    // copy_video_frame() runs on the decode thread.
    ComPtr<ID3D10Multithread> mt;
    ctx_.As(&mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgi_device;
    device_.As(&dxgi_device);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_device->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    // The two present modes want opposite things from the swapchain, so the
    // flags are decided here and reused verbatim by ResizeBuffers.
    if (!waitable_) {
        ComPtr<IDXGIFactory5> factory5;
        factory.As(&factory5);
        if (factory5) {
            BOOL allowed = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                        &allowed, sizeof(allowed)))) {
                tearing_ = allowed == TRUE;
            }
        }
        swap_flags_ = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    } else {
        swap_flags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = swap_flags_;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        KRG_LOG("swapchain creation failed (hr=0x%08lX)", hr);
        return false;
    }

    if (waitable_) {
        ComPtr<IDXGISwapChain2> swap2;
        if (SUCCEEDED(swap_.As(&swap2))) {
            // One frame of latency: the wait returns as the display finishes
            // with a buffer, so rendering starts as late as it can and still
            // make the next vblank.
            swap2->SetMaximumFrameLatency(1);
            frame_latency_waitable_ = swap2->GetFrameLatencyWaitableObject();
        }
        if (!frame_latency_waitable_) {
            KRG_LOG("frame latency waitable object unavailable, pacing on Present alone");
        }
    }

    ComPtr<ID3D11Texture2D> backbuffer;
    swap_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv_);
    // ResizeBuffers demands zero outstanding backbuffer references, and
    // ShowWindow below delivers a WM_SIZE synchronously — drop ours now.
    backbuffer.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = frame_w_;
    td.Height = frame_h_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (video_mode_) {
        td.Format = DXGI_FORMAT_NV12;
        hr = device_->CreateTexture2D(&td, nullptr, &nv12_tex_);
        if (FAILED(hr)) {
            KRG_LOG("NV12 texture creation failed (hr=0x%08lX)", hr);
            return false;
        }
        // NV12 is sampled as two planes: R8 luma and R8G8 chroma.
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        sv.Format = DXGI_FORMAT_R8_UNORM;
        KRG_HRB(device_->CreateShaderResourceView(nv12_tex_.Get(), &sv, &nv12_y_srv_));
        sv.Format = DXGI_FORMAT_R8G8_UNORM;
        KRG_HRB(device_->CreateShaderResourceView(nv12_tex_.Get(), &sv, &nv12_uv_srv_));
    } else {
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        hr = device_->CreateTexture2D(&td, nullptr, &frame_tex_);
        if (FAILED(hr)) {
            KRG_LOG("frame texture creation failed (hr=0x%08lX)", hr);
            return false;
        }
        device_->CreateShaderResourceView(frame_tex_.Get(), nullptr, &frame_srv_);
    }

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
        ComPtr<ID3DBlob> errors;
        HRESULT chr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr,
                                 entry, target, 0, 0, &blob, &errors);
        if (FAILED(chr)) {
            KRG_LOG("%s compile failed: %s", entry,
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> blob;
    if (!compile("vs_main", "vs_5_0", blob)) return false;
    device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs_);
    if (!compile("ps_main", "ps_5_0", blob)) return false;
    device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps_);
    if (!compile("ps_nv12", "ps_5_0", blob)) return false;
    device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                               &ps_nv12_);
    if (!compile("vs_cursor", "vs_5_0", blob)) return false;
    device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                &vs_cursor_);
    if (!compile("ps_cursor", "ps_5_0", blob)) return false;
    device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                               &ps_cursor_);
    if (!compile("ps_cursor_nv12", "ps_5_0", blob)) return false;
    device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                               &ps_cursor_nv12_);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32; // CursorCB: two float4s
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    KRG_HRB(device_->CreateBuffer(&bd, nullptr, &cursor_cb_));

    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&samp, &sampler_);

    if (stats_ && !init_overlay()) {
        KRG_LOG("stats overlay unavailable, continuing without it");
        stats_ = false;
    }

    ShowWindow(hwnd_, SW_SHOW);
    KRG_LOG("presenting %ux%u (%s)", frame_w_, frame_h_,
            waitable_ ? "waitable swapchain, 1 frame of latency"
                      : (tearing_ ? "tearing allowed" : "tearing unsupported"));
    return true;
}

bool Presenter::init_overlay() {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr,
                          "ps_overlay", "ps_5_0", 0, 0, &blob, &errors))) {
        KRG_LOG("ps_overlay compile failed");
        return false;
    }
    KRG_HRB(device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                       &ps_overlay_));

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    KRG_HRB(device_->CreateBlendState(&bd, &overlay_blend_));

    // Text drawn at 1:1 and sampled with a linear filter picks up a half-texel
    // smear; point sampling keeps the glyphs as crisp as GDI rendered them.
    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    KRG_HRB(device_->CreateSamplerState(&samp, &point_sampler_));

    // Sized for a dozen lines at whatever this monitor's DPI is; the quad only
    // covers the sub-rect the text actually filled.
    const UINT dpi = GetDpiForWindow(hwnd_);
    const int font_height = MulDiv(15, static_cast<int>(dpi ? dpi : 96), 96);
    overlay_tex_w_ = static_cast<uint32_t>(MulDiv(360, static_cast<int>(dpi ? dpi : 96), 96));
    overlay_tex_h_ = static_cast<uint32_t>(font_height * 14);

    overlay_dc_ = CreateCompatibleDC(nullptr);
    if (!overlay_dc_) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(overlay_tex_w_);
    bi.bmiHeader.biHeight = -static_cast<LONG>(overlay_tex_h_); // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    overlay_bmp_ = CreateDIBSection(overlay_dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!overlay_bmp_ || !bits) return false;
    overlay_bits_ = static_cast<uint8_t*>(bits);
    overlay_old_bmp_ = SelectObject(overlay_dc_, overlay_bmp_);

    overlay_font_ = CreateFontA(-font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    if (overlay_font_) SelectObject(overlay_dc_, overlay_font_);
    SetTextColor(overlay_dc_, RGB(255, 255, 255));
    SetBkMode(overlay_dc_, TRANSPARENT);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = overlay_tex_w_;
    td.Height = overlay_tex_h_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    KRG_HRB(device_->CreateTexture2D(&td, nullptr, &overlay_tex_));
    KRG_HRB(device_->CreateShaderResourceView(overlay_tex_.Get(), nullptr, &overlay_srv_));
    return true;
}

void Presenter::set_stats_text(const char* text) {
    if (!stats_ || !overlay_bits_) return;

    const size_t stride = size_t{overlay_tex_w_} * 4;
    std::memset(overlay_bits_, 0, stride * overlay_tex_h_);

    RECT rc{kOverlayPad, kOverlayPad, static_cast<LONG>(overlay_tex_w_) - kOverlayPad,
            static_cast<LONG>(overlay_tex_h_) - kOverlayPad};
    const int len = static_cast<int>(std::strlen(text));
    const UINT flags = DT_LEFT | DT_TOP | DT_NOPREFIX;
    RECT measured = rc;
    DrawTextA(overlay_dc_, text, len, &measured, flags | DT_CALCRECT);
    DrawTextA(overlay_dc_, text, len, &rc, flags);
    GdiFlush();

    overlay_used_w_ = std::min<uint32_t>(overlay_tex_w_, measured.right + kOverlayPad);
    overlay_used_h_ = std::min<uint32_t>(overlay_tex_h_, measured.bottom + kOverlayPad);

    // GDI never writes the alpha channel, so derive it: the glyphs are white on
    // a cleared bitmap, and everything inside the used rect gets at least the
    // backing box's alpha so the text stays readable over bright content.
    for (uint32_t y = 0; y < overlay_used_h_; ++y) {
        uint8_t* row = overlay_bits_ + y * stride;
        for (uint32_t x = 0; x < overlay_used_w_; ++x) {
            uint8_t* px = row + size_t{x} * 4;
            uint8_t coverage = std::max({px[0], px[1], px[2]});
            px[3] = static_cast<uint8_t>(kOverlayBoxAlpha +
                                         coverage * (255 - kOverlayBoxAlpha) / 255);
        }
    }

    D3D11_BOX box{0, 0, 0, overlay_used_w_, overlay_used_h_, 1};
    ctx_->UpdateSubresource(overlay_tex_.Get(), 0, &box, overlay_bits_,
                            static_cast<UINT>(stride), 0);
}

void Presenter::handle_resize(uint32_t w, uint32_t h) {
    if (!swap_ || w == 0 || h == 0) return;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swap_flags_);
    if (FAILED(hr)) KRG_LOG("ResizeBuffers failed (hr=0x%08lX)", hr);
    // Recreate the RTV even if the resize failed: presenting at the old
    // backbuffer size (DXGI stretches) beats never presenting again.
    ComPtr<ID3D11Texture2D> backbuffer;
    swap_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (backbuffer) device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv_);
    present();
}

bool Presenter::pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return true;
}

void Presenter::apply(const std::vector<RectUpdate>& updates) {
    for (const RectUpdate& u : updates) {
        D3D11_BOX box{u.rect.x, u.rect.y, 0, u.rect.x + u.rect.w, u.rect.y + u.rect.h, 1};
        ctx_->UpdateSubresource(frame_tex_.Get(), 0, &box, u.pixels.data(), u.rect.w * 4, 0);
    }
}

void Presenter::set_cursor_pos(int32_t x, int32_t y, bool visible) {
    std::lock_guard lock(cursor_mutex_);
    cursor_x_ = x;
    cursor_y_ = y;
    cursor_visible_ = visible;
}

void Presenter::set_cursor_shape(uint32_t w, uint32_t h, const uint8_t* bgra,
                                 const uint8_t* invert) {
    std::lock_guard lock(cursor_mutex_);
    if (w != cursor_w_ || h != cursor_h_ || !cursor_tex_) {
        cursor_tex_.Reset();
        cursor_srv_.Reset();
        cursor_inv_tex_.Reset();
        cursor_inv_srv_.Reset();
        cursor_w_ = cursor_h_ = 0;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &cursor_tex_))) return;
        device_->CreateShaderResourceView(cursor_tex_.Get(), nullptr, &cursor_srv_);
        td.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &cursor_inv_tex_))) return;
        device_->CreateShaderResourceView(cursor_inv_tex_.Get(), nullptr, &cursor_inv_srv_);
        cursor_w_ = w;
        cursor_h_ = h;
    }
    ctx_->UpdateSubresource(cursor_tex_.Get(), 0, nullptr, bgra, w * 4, 0);
    ctx_->UpdateSubresource(cursor_inv_tex_.Get(), 0, nullptr, invert, w, 0);
}

void Presenter::copy_video_frame(ID3D11Texture2D* nv12, UINT subresource) {
    if (!nv12_tex_) return;
    // Decoder textures may be padded to macroblock alignment; copy only our
    // frame's region. The box also crops the chroma plane correspondingly.
    D3D11_BOX box{0, 0, 0, frame_w_, frame_h_, 1};
    ctx_->CopySubresourceRegion(nv12_tex_.Get(), 0, 0, 0, 0, nv12, subresource, &box);
}

void Presenter::present() {
    if (!rtv_) return;
    // Blocks until the display is done with a buffer. Doing it here rather
    // than in the caller's loop keeps the wait next to the Present it paces,
    // and the message pump runs either side of it.
    if (frame_latency_waitable_) WaitForSingleObjectEx(frame_latency_waitable_, 100, TRUE);

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(rc.right - rc.left);
    vp.Height = static_cast<float>(rc.bottom - rc.top);
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);

    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    if (video_mode_) {
        ctx_->PSSetShader(ps_nv12_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = {nv12_y_srv_.Get(), nv12_uv_srv_.Get()};
        ctx_->PSSetShaderResources(0, 2, srvs);
    } else {
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = {frame_srv_.Get()};
        ctx_->PSSetShaderResources(0, 1, srvs);
    }
    ID3D11SamplerState* samplers[] = {sampler_.Get()};
    ctx_->PSSetSamplers(0, 1, samplers);
    ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
    ctx_->OMSetRenderTargets(1, rtvs, nullptr);
    ctx_->Draw(3, 0);

    {
        std::lock_guard lock(cursor_mutex_);
        if (cursor_visible_ && cursor_srv_ && cursor_inv_srv_) {
            // Cursor rect in frame-normalized coords; the frame fills the
            // whole viewport, so NDC follows directly and the cursor scales
            // with the window like everything else.
            float x0 = cursor_x_ / static_cast<float>(frame_w_);
            float y0 = cursor_y_ / static_cast<float>(frame_h_);
            float x1 = (cursor_x_ + cursor_w_) / static_cast<float>(frame_w_);
            float y1 = (cursor_y_ + cursor_h_) / static_cast<float>(frame_h_);
            float cb[8] = {x0 * 2 - 1, 1 - y0 * 2, x1 * 2 - 1, 1 - y1 * 2, x0, y0, x1, y1};
            ctx_->UpdateSubresource(cursor_cb_.Get(), 0, nullptr, cb, 0, 0);

            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx_->VSSetShader(vs_cursor_.Get(), nullptr, 0);
            ID3D11Buffer* cbs[] = {cursor_cb_.Get()};
            ctx_->VSSetConstantBuffers(0, 1, cbs);
            // The frame SRVs from the main draw stay bound at t0/t1 as the
            // background the cursor shader blends against.
            ctx_->PSSetShader(video_mode_ ? ps_cursor_nv12_.Get() : ps_cursor_.Get(), nullptr, 0);
            ID3D11ShaderResourceView* csrvs[] = {cursor_srv_.Get(), cursor_inv_srv_.Get()};
            ctx_->PSSetShaderResources(2, 2, csrvs);
            ctx_->Draw(4, 0);
        }
    }

    if (stats_) draw_overlay(rc);

    // Minimal mode: sync interval 0 + allow-tearing, never blocking on vblank.
    // Smooth mode: sync interval 1, with the wait above providing the pacing.
    HRESULT hr = waitable_ ? swap_->Present(1, 0)
                           : swap_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    last_present_us_ = now_us();
    if (FAILED(hr) && !present_error_logged_) {
        KRG_LOG("Present failed (hr=0x%08lX)", hr);
        present_error_logged_ = true;
    }
    if (stats_) {
        UINT count = 0;
        if (SUCCEEDED(swap_->GetLastPresentCount(&count))) {
            present_log_[present_log_next_] = {count, last_present_us_};
            present_log_next_ = (present_log_next_ + 1) % std::size(present_log_);
        }
    }
    drain_debug_messages();
}

void Presenter::draw_overlay(const RECT& client) {
    if (!overlay_srv_ || !overlay_used_w_) return;
    const float cw = static_cast<float>(client.right - client.left);
    const float ch = static_cast<float>(client.bottom - client.top);
    if (cw <= 0 || ch <= 0) return;

    // Drawn at 1:1 with the backbuffer so the glyphs land on whole pixels,
    // which means the box does not scale with the window the way the cursor
    // does — deliberately, since it is instrumentation, not content.
    const float x0 = kOverlayMargin, y0 = kOverlayMargin;
    const float x1 = x0 + overlay_used_w_, y1 = y0 + overlay_used_h_;
    const float u1 = static_cast<float>(overlay_used_w_) / overlay_tex_w_;
    const float v1 = static_cast<float>(overlay_used_h_) / overlay_tex_h_;
    float cb[8] = {x0 / cw * 2 - 1, 1 - y0 / ch * 2, x1 / cw * 2 - 1, 1 - y1 / ch * 2,
                   0,               0,               u1,              v1};
    ctx_->UpdateSubresource(cursor_cb_.Get(), 0, nullptr, cb, 0, 0);

    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_cursor_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {cursor_cb_.Get()};
    ctx_->VSSetConstantBuffers(0, 1, cbs);
    ctx_->PSSetShader(ps_overlay_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {overlay_srv_.Get()};
    ctx_->PSSetShaderResources(4, 1, srvs);
    ID3D11SamplerState* samplers[] = {point_sampler_.Get()};
    ctx_->PSSetSamplers(0, 1, samplers);
    ctx_->OMSetBlendState(overlay_blend_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->Draw(4, 0);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

double Presenter::scanout_delay_ms() {
    DXGI_FRAME_STATISTICS fs{};
    if (FAILED(swap_->GetFrameStatistics(&fs)) || fs.SyncQPCTime.QuadPart == 0) {
        return last_scanout_ms_;
    }
    for (const PresentRecord& record : present_log_) {
        if (record.us == 0 || record.count != fs.PresentCount) continue;
        last_scanout_ms_ = (qpc_to_us(fs.SyncQPCTime.QuadPart) - record.us) / 1000.0;
        break;
    }
    return last_scanout_ms_;
}

void Presenter::drain_debug_messages() {
    if (!info_queue_) return;
    UINT64 count = info_queue_->GetNumStoredMessages();
    std::vector<uint8_t> buf;
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T len = 0;
        if (FAILED(info_queue_->GetMessage(i, nullptr, &len))) continue;
        buf.resize(len);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (SUCCEEDED(info_queue_->GetMessage(i, msg, &len))) {
            KRG_LOG("d3d[%d]: %.*s", static_cast<int>(msg->Severity),
                    static_cast<int>(msg->DescriptionByteLength), msg->pDescription);
        }
    }
    info_queue_->ClearStoredMessages();
}

} // namespace krg
