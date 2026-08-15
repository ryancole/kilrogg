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

bool convert_cursor_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si, const uint8_t* data,
                          CursorShape& out) {
    switch (si.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
        out.width = si.Width;
        out.height = si.Height;
        out.bgra.resize(size_t{out.width} * out.height * 4);
        out.invert.assign(size_t{out.width} * out.height, 0);
        for (uint32_t y = 0; y < out.height; ++y) {
            std::memcpy(out.bgra.data() + size_t{y} * out.width * 4,
                        data + size_t{y} * si.Pitch, size_t{out.width} * 4);
        }
        return true;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
        // 1bpp MSB-first AND mask stacked on top of a 1bpp XOR mask.
        // AND=0 draws the XOR bit as black/white; AND=1 + XOR=1 inverts the
        // screen (the I-beam is made entirely of these); AND=1 + XOR=0 is
        // transparent.
        out.width = si.Width;
        out.height = si.Height / 2;
        out.bgra.assign(size_t{out.width} * out.height * 4, 0);
        out.invert.assign(size_t{out.width} * out.height, 0);
        for (uint32_t y = 0; y < out.height; ++y) {
            const uint8_t* and_row = data + size_t{y} * si.Pitch;
            const uint8_t* xor_row = data + size_t{y + out.height} * si.Pitch;
            for (uint32_t x = 0; x < out.width; ++x) {
                bool and_bit = (and_row[x / 8] >> (7 - x % 8)) & 1;
                bool xor_bit = (xor_row[x / 8] >> (7 - x % 8)) & 1;
                size_t i = size_t{y} * out.width + x;
                if (!and_bit) {
                    out.bgra[i * 4 + 0] = out.bgra[i * 4 + 1] = out.bgra[i * 4 + 2] =
                        xor_bit ? 255 : 0;
                    out.bgra[i * 4 + 3] = 255;
                } else if (xor_bit) {
                    out.invert[i] = 255;
                }
            }
        }
        return true;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
        // 32bpp; alpha byte 0xFF means XOR the color with the screen (black
        // XOR = transparent, anything else approximated as invert), else the
        // color is opaque.
        out.width = si.Width;
        out.height = si.Height;
        out.bgra.assign(size_t{out.width} * out.height * 4, 0);
        out.invert.assign(size_t{out.width} * out.height, 0);
        for (uint32_t y = 0; y < out.height; ++y) {
            const uint8_t* row = data + size_t{y} * si.Pitch;
            for (uint32_t x = 0; x < out.width; ++x) {
                const uint8_t* src = row + size_t{x} * 4;
                size_t i = size_t{y} * out.width + x;
                if (src[3] == 0xFF) {
                    if (src[0] || src[1] || src[2]) out.invert[i] = 255;
                } else {
                    out.bgra[i * 4 + 0] = src[0];
                    out.bgra[i * 4 + 1] = src[1];
                    out.bgra[i * 4 + 2] = src[2];
                    out.bgra[i * 4 + 3] = 255;
                }
            }
        }
        return true;
    }
    }
    return false;
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

void DxgiCapture::update_cursor(const DXGI_OUTDUPL_FRAME_INFO& info) {
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        std::lock_guard lock(cursor_mutex_);
        cursor_pos_.x = info.PointerPosition.Position.x;
        cursor_pos_.y = info.PointerPosition.Position.y;
        // Visible is FALSE when the cursor is hidden, on another output, or
        // already composited into the desktop image (software cursor) — in
        // all of those cases the receiver must not draw its own.
        cursor_pos_.visible = info.PointerPosition.Visible != FALSE;
        ++cursor_pos_version_;
    }

    if (info.PointerShapeBufferSize == 0) return; // shape unchanged
    shape_buf_.resize(info.PointerShapeBufferSize);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    UINT needed = 0;
    HRESULT hr = dup_->GetFramePointerShape(static_cast<UINT>(shape_buf_.size()),
                                            shape_buf_.data(), &needed, &si);
    if (FAILED(hr)) {
        KRG_LOG("GetFramePointerShape failed (hr=0x%08lX)", hr);
        return;
    }
    CursorShape shape;
    if (!convert_cursor_shape(si, shape_buf_.data(), shape)) return;
    std::lock_guard lock(cursor_mutex_);
    cursor_shape_ = std::move(shape);
    ++cursor_shape_version_;
}

bool DxgiCapture::poll_cursor_pos(uint64_t& last_version, CursorPos& out) {
    std::lock_guard lock(cursor_mutex_);
    if (cursor_pos_version_ == last_version) return false;
    last_version = cursor_pos_version_;
    out = cursor_pos_;
    return true;
}

bool DxgiCapture::poll_cursor_shape(uint64_t& last_version, CursorShape& out) {
    std::lock_guard lock(cursor_mutex_);
    if (cursor_shape_version_ == last_version) return false;
    last_version = cursor_shape_version_;
    out = cursor_shape_;
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
    // Zero timeout + short sleep instead of a blocking wait: blocking inside
    // AcquireNextFrame holds up other users of this device (the encoder's
    // GPU submissions), adding hundreds of ms of latency.
    HRESULT hr = dup_->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        Sleep(2); // static screen — normal, not an error
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        // ACCESS_LOST: mode switch, secure desktop, etc. INVALID_CALL: the
        // duplication is poisoned — seen when a game engages exclusive
        // fullscreen, especially on hybrid-GPU machines. Recreate it; the
        // next successful frame is a full-frame keyframe either way.
        Sleep(100);
        reinit_duplication();
        return false;
    }
    if (FAILED(hr)) {
        KRG_LOG("AcquireNextFrame failed (hr=0x%08lX)", hr);
        Sleep(100);
        return false;
    }

    update_cursor(info);

    rects.clear();
    collect_rects(info, rects);

    // Only the mouse pointer changed: no pixels to send. The cursor state
    // recorded above still reaches the client as a cursor packet.
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
