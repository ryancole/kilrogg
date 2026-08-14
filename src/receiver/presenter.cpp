#include "receiver/presenter.h"

#include <algorithm>

#include <d3dcompiler.h>

#include "common/log.h"

using Microsoft::WRL::ComPtr;

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
)";

constexpr char kWndClass[] = "KilroggWindow";

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

std::unique_ptr<Presenter> Presenter::create(uint32_t frame_width, uint32_t frame_height) {
    std::unique_ptr<Presenter> p(new Presenter());
    if (!p->init(frame_width, frame_height)) return nullptr;
    return p;
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

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    if (FAILED(hr)) {
        KRG_LOG("D3D11CreateDevice failed (hr=0x%08lX)", hr);
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    device_.As(&dxgi_device);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_device->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    ComPtr<IDXGIFactory5> factory5;
    factory.As(&factory5);
    if (factory5) {
        BOOL allowed = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &allowed, sizeof(allowed)))) {
            tearing_ = allowed == TRUE;
        }
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        KRG_LOG("swapchain creation failed (hr=0x%08lX)", hr);
        return false;
    }

    ComPtr<ID3D11Texture2D> backbuffer;
    swap_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv_);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = frame_w_;
    td.Height = frame_h_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = device_->CreateTexture2D(&td, nullptr, &frame_tex_);
    if (FAILED(hr)) {
        KRG_LOG("frame texture creation failed (hr=0x%08lX)", hr);
        return false;
    }
    device_->CreateShaderResourceView(frame_tex_.Get(), nullptr, &frame_srv_);

    ComPtr<ID3DBlob> blob, errors;
    hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr, "vs_main",
                    "vs_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) {
        KRG_LOG("vertex shader compile failed: %s",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs_);

    blob.Reset();
    errors.Reset();
    hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr, "ps_main",
                    "ps_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) {
        KRG_LOG("pixel shader compile failed: %s",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps_);

    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&samp, &sampler_);

    ShowWindow(hwnd_, SW_SHOW);
    KRG_LOG("presenting %ux%u (tearing %s)", frame_w_, frame_h_,
            tearing_ ? "allowed" : "unsupported");
    return true;
}

void Presenter::handle_resize(uint32_t w, uint32_t h) {
    if (!swap_ || w == 0 || h == 0) return;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
                                      tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        KRG_LOG("ResizeBuffers failed (hr=0x%08lX)", hr);
        return;
    }
    ComPtr<ID3D11Texture2D> backbuffer;
    swap_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv_);
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

void Presenter::present() {
    if (!rtv_) return;
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
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {frame_srv_.Get()};
    ctx_->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samplers[] = {sampler_.Get()};
    ctx_->PSSetSamplers(0, 1, samplers);
    ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
    ctx_->OMSetRenderTargets(1, rtvs, nullptr);
    ctx_->Draw(3, 0);

    // Sync interval 0 + allow-tearing: never block on vblank.
    swap_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

} // namespace krg
