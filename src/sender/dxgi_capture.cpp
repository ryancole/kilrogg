#include "sender/dxgi_capture.h"

#include <cstring>

#include "common/log.h"

using Microsoft::WRL::ComPtr;

namespace krg {

namespace {
Rect rect_from(const RECT& r) {
    Rect out;
    out.x = static_cast<uint32_t>(r.left < 0 ? 0 : r.left);
    out.y = static_cast<uint32_t>(r.top < 0 ? 0 : r.top);
    out.w = static_cast<uint32_t>(r.right - r.left);
    out.h = static_cast<uint32_t>(r.bottom - r.top);
    return out;
}
} // namespace

std::unique_ptr<DxgiCapture> DxgiCapture::create() {
    std::unique_ptr<DxgiCapture> cap(new DxgiCapture());
    if (!cap->init()) return nullptr;
    return cap;
}

bool DxgiCapture::init() {
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_, &fl, &context_);
    if (FAILED(hr)) {
        KRG_LOG("D3D11CreateDevice failed (hr=0x%08lX)", hr);
        return false;
    }

    // The video path uses this device from the capture, send, and MF worker
    // threads concurrently.
    ComPtr<ID3D10Multithread> mt;
    context_.As(&mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgi_device;
    device_.As(&dxgi_device);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_device->GetAdapter(&adapter);

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(0, &output);
    if (FAILED(hr)) {
        KRG_LOG("no display output found (hr=0x%08lX)", hr);
        return false;
    }
    output.As(&output_);
    if (!output_) {
        KRG_LOG("IDXGIOutput1 unavailable — Desktop Duplication needs Windows 8+");
        return false;
    }

    if (!reinit_duplication()) return false;

    DXGI_OUTDUPL_DESC desc{};
    dup_->GetDesc(&desc);
    width_ = desc.ModeDesc.Width;
    height_ = desc.ModeDesc.Height;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device_->CreateTexture2D(&td, nullptr, &staging_);
    if (FAILED(hr)) {
        KRG_LOG("staging texture creation failed (hr=0x%08lX)", hr);
        return false;
    }

    KRG_LOG("capturing primary output at %ux%u", width_, height_);
    return true;
}

bool DxgiCapture::reinit_duplication() {
    dup_.Reset();
    HRESULT hr = output_->DuplicateOutput(device_.Get(), &dup_);
    if (FAILED(hr)) {
        if (hr == E_ACCESSDENIED) {
            KRG_LOG("DuplicateOutput access denied (secure desktop / UAC prompt active?)");
        } else {
            KRG_LOG("DuplicateOutput failed (hr=0x%08lX)", hr);
        }
        return false;
    }

    DXGI_OUTDUPL_DESC desc{};
    dup_->GetDesc(&desc);
    if (width_ != 0 && (desc.ModeDesc.Width != width_ || desc.ModeDesc.Height != height_)) {
        KRG_LOG("display resolution changed (%ux%u -> %ux%u); not supported yet, exiting",
                width_, height_, desc.ModeDesc.Width, desc.ModeDesc.Height);
        std::exit(1);
    }

    first_frame_ = true;
    return true;
}

void DxgiCapture::collect_rects(const DXGI_OUTDUPL_FRAME_INFO& info, std::vector<Rect>& rects) {
    if (first_frame_ || info.TotalMetadataBufferSize == 0) {
        if (first_frame_) rects.push_back({0, 0, width_, height_});
        return;
    }

    metadata_.resize(info.TotalMetadataBufferSize);

    UINT move_bytes = 0;
    HRESULT hr = dup_->GetFrameMoveRects(static_cast<UINT>(metadata_.size()),
                                         reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadata_.data()),
                                         &move_bytes);
    if (FAILED(hr)) {
        rects.assign(1, {0, 0, width_, height_}); // can't trust metadata: resend everything
        return;
    }
    auto* moves = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadata_.data());
    for (UINT i = 0; i < move_bytes / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) {
        // We always send pixels from the full desktop copy, so marking the
        // destination dirty fully covers a move.
        rects.push_back(rect_from(moves[i].DestinationRect));
    }

    UINT dirty_bytes = 0;
    hr = dup_->GetFrameDirtyRects(static_cast<UINT>(metadata_.size()),
                                  reinterpret_cast<RECT*>(metadata_.data()), &dirty_bytes);
    if (FAILED(hr)) {
        rects.assign(1, {0, 0, width_, height_});
        return;
    }
    auto* dirty = reinterpret_cast<RECT*>(metadata_.data());
    for (UINT i = 0; i < dirty_bytes / sizeof(RECT); ++i) rects.push_back(rect_from(dirty[i]));
}

bool DxgiCapture::acquire(ComPtr<ID3D11Texture2D>& acquired, bool& have_rects,
                          std::vector<Rect>& rects) {
    have_rects = false;
    if (!dup_ && !reinit_duplication()) {
        Sleep(500); // avoid a hot spin while the desktop is unavailable
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dup_->AcquireNextFrame(100, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false; // static screen — normal, not an error
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Mode switch, secure desktop, etc. Recreate and resend a keyframe.
        reinit_duplication();
        return false;
    }
    if (FAILED(hr)) {
        KRG_LOG("AcquireNextFrame failed (hr=0x%08lX)", hr);
        Sleep(100);
        return false;
    }

    rects.clear();
    collect_rects(info, rects);

    // Only the mouse pointer changed: nothing to send (no cursor overlay yet).
    if (rects.empty()) {
        dup_->ReleaseFrame();
        return false;
    }

    resource.As(&acquired);
    have_rects = true;
    return true;
}

bool DxgiCapture::next_frame_texture(ComPtr<ID3D11Texture2D>& out) {
    ComPtr<ID3D11Texture2D> acquired;
    bool have_rects = false;
    std::vector<Rect> rects;
    if (!acquire(acquired, have_rects, rects)) return false;

    if (!pool_[0]) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width_;
        td.Height = height_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (auto& tex : pool_) {
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) {
                KRG_LOG("capture pool texture creation failed");
                dup_->ReleaseFrame();
                return false;
            }
        }
    }

    ComPtr<ID3D11Texture2D>& slot = pool_[pool_index_ % std::size(pool_)];
    ++pool_index_;
    context_->CopyResource(slot.Get(), acquired.Get());
    dup_->ReleaseFrame();
    out = slot;
    first_frame_ = false;
    return true;
}

bool DxgiCapture::next_frame(Frame& out) {
    ComPtr<ID3D11Texture2D> acquired;
    bool have_rects = false;
    std::vector<Rect> rects;
    if (!acquire(acquired, have_rects, rects)) return false;

    context_->CopyResource(staging_.Get(), acquired.Get());
    dup_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE map{};
    HRESULT hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) {
        KRG_LOG("staging map failed (hr=0x%08lX)", hr);
        return false;
    }
    out.width = width_;
    out.height = height_;
    out.pixels.resize(size_t{width_} * height_ * 4);
    const auto* src = static_cast<const uint8_t*>(map.pData);
    for (uint32_t y = 0; y < height_; ++y) {
        std::memcpy(out.pixels.data() + size_t{y} * width_ * 4,
                    src + size_t{y} * map.RowPitch, size_t{width_} * 4);
    }
    context_->Unmap(staging_.Get(), 0);

    out.rects = std::move(rects);
    out.id = ++frame_id_;
    first_frame_ = false;
    return true;
}

} // namespace krg
