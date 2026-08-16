#include "sender/dxgi_capture.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"

using Microsoft::WRL::ComPtr;

namespace krg {

namespace {

// How fast the desktop is actually being redrawn, which is the rate the encoder
// has to be told about: it budgets its bits per frame from the frame rate it is
// configured with, so a 165 Hz desktop encoded as if it were 60 spends most of
// three times the commanded bitrate.
//
// The duplication's own mode description is the first answer, rounded to whole
// Hz — 143.998 and 144 are the same decision, and comparing exact rationals
// would renegotiate the connection every time a driver reported it a hair
// differently. Not every driver fills it in, hence the second opinion, which
// has to name the display being captured: a null device name there is the
// primary, and with --display that is routinely some other screen running at
// some other rate.
uint32_t refresh_hz_from(const DXGI_MODE_DESC& mode, const std::string& device_name) {
    if (mode.RefreshRate.Numerator && mode.RefreshRate.Denominator) {
        const uint32_t d = mode.RefreshRate.Denominator;
        return std::max(1u, (mode.RefreshRate.Numerator + d / 2) / d);
    }
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    // 0 and 1 both mean "the hardware default" rather than a rate.
    if (EnumDisplaySettingsA(device_name.empty() ? nullptr : device_name.c_str(),
                             ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1) {
        return dm.dmDisplayFrequency;
    }
    return 60;
}

const char* format_name(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "scRGB half-float (HDR)";
    default: return "an unrecognised format";
    }
}

Rect rect_from(const RECT& r) {
    Rect out;
    out.x = static_cast<uint32_t>(r.left < 0 ? 0 : r.left);
    out.y = static_cast<uint32_t>(r.top < 0 ? 0 : r.top);
    out.w = static_cast<uint32_t>(r.right - r.left);
    out.h = static_cast<uint32_t>(r.bottom - r.top);
    return out;
}

} // namespace

std::unique_ptr<DxgiCapture> DxgiCapture::create(const std::string& display_selector) {
    const DisplayList displays = enumerate_displays();
    const DisplayDevice* chosen = select_display(displays, display_selector);
    if (!chosen) return nullptr;
    std::unique_ptr<DxgiCapture> cap(new DxgiCapture());
    if (!cap->init(*chosen)) return nullptr;
    return cap;
}

bool DxgiCapture::init(const DisplayDevice& display) {
    display_ = display;

    D3D_FEATURE_LEVEL fl{};
    // This device backs the encoder's DXGI device manager and the video
    // processor doing BGRA->NV12, both of which are documented to require
    // VIDEO_SUPPORT — it has worked without it on the drivers tried so far,
    // which is not the same as being allowed to. BGRA_SUPPORT goes with it, and
    // the dummy source's device already asks for both. A machine whose driver
    // refuses them has no video path at all, but --codec lz4 still works, so
    // that is a fallback rather than a failure.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    // On the adapter that owns the chosen output, because Desktop Duplication
    // only works between the two — and because the device made here is the one
    // the encoder and the video processor are handed, so this is the line that
    // decides which GPU the whole sender runs on. DRIVER_TYPE_UNKNOWN is not a
    // relaxation: naming an adapter and a driver type at once is an error, and
    // the adapter already says what it is.
    HRESULT hr = D3D11CreateDevice(display_.adapter_obj.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   flags, nullptr, 0, D3D11_SDK_VERSION, &device_, &fl, &context_);
    if (FAILED(hr)) {
        KRG_LOG("no video-capable D3D11 device on %s (hr=0x%08lX), falling back — only "
                "--codec lz4 will work", display_.adapter.c_str(), hr);
        hr = D3D11CreateDevice(display_.adapter_obj.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                               nullptr, 0, D3D11_SDK_VERSION, &device_, &fl, &context_);
    }
    if (FAILED(hr)) {
        KRG_LOG("D3D11CreateDevice failed on %s (hr=0x%08lX)", display_.adapter.c_str(), hr);
        return false;
    }

    // The video path uses this device from the capture, send, and MF worker
    // threads concurrently.
    ComPtr<ID3D10Multithread> mt;
    context_.As(&mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    display_.output_obj.As(&output_);
    if (!output_) {
        KRG_LOG("IDXGIOutput1 unavailable — Desktop Duplication needs Windows 8+");
        return false;
    }
    display_.output_obj.As(&output5_); // optional; see reinit_duplication

    if (!reinit_duplication()) return false;

    KRG_LOG("capturing %s at %ux%u @%uHz, %s", display_.label().c_str(), width(), height(),
            refresh_hz(), format_name(format()));
    return true;
}

bool DxgiCapture::reinit_duplication() {
    dup_.Reset();

    // Ask for BGRA8 rather than take what the desktop happens to be in. With
    // HDR switched on the desktop composites as scRGB half-float, and nothing
    // downstream can use that: the copy into the capture pool would be a
    // format mismatch, which D3D11 answers by doing nothing at all rather than
    // by failing, and the GPU's video processor will not take a float surface
    // as input to convert either (measured on an RTX 4090: input unsupported,
    // CreateVideoProcessorInputView returns E_INVALIDARG). DuplicateOutput1
    // takes a list of formats the caller can accept and has DXGI do the
    // conversion, which is both less code here and Windows' own HDR-to-SDR
    // mapping rather than one guessed at in a shader.
    HRESULT hr = E_NOINTERFACE;
    if (output5_) {
        const DXGI_FORMAT accepted[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5_->DuplicateOutput1(device_.Get(), 0, 1, accepted, &dup_);
        if (FAILED(hr) && hr != E_ACCESSDENIED) {
            KRG_LOG("DuplicateOutput1 failed (hr=0x%08lX), falling back to the untyped "
                    "duplication", hr);
        }
    }
    if (FAILED(hr)) hr = output_->DuplicateOutput(device_.Get(), &dup_);
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
    // The format here is a seed rather than the last word: the first acquired
    // texture is what the copies actually have to match, and acquire() corrects
    // this from it if the two ever disagree.
    adopt_mode(desc.ModeDesc.Width, desc.ModeDesc.Height,
               refresh_hz_from(desc.ModeDesc, display_.device), desc.ModeDesc.Format);

    first_frame_ = true;
    return true;
}

void DxgiCapture::adopt_mode(uint32_t new_width, uint32_t new_height, uint32_t new_hz,
                             DXGI_FORMAT new_format) {
    const uint32_t old_w = width_.load(std::memory_order_relaxed);
    const uint32_t old_h = height_.load(std::memory_order_relaxed);
    const uint32_t old_hz = refresh_hz_.load(std::memory_order_relaxed);
    const auto old_format = static_cast<DXGI_FORMAT>(format_.load(std::memory_order_relaxed));
    if (new_width == old_w && new_height == old_h && new_hz == old_hz &&
        new_format == old_format) {
        return;
    }

    // The pool and staging textures are cut to the old mode and the encoder on
    // the other side of the mailbox is configured for it, so both go. They are
    // recreated lazily at the new size on the next frame; the flag is what
    // tells the run loop to rebuild the encoder and the connection to match.
    // A refresh rate change alone leaves the textures the right size, but the
    // encoder is configured for the old rate just the same, so it renegotiates
    // too — dropping a client for a fraction of a second beats encoding a
    // 165 Hz desktop against a 60 fps bit budget.
    for (auto& tex : pool_) tex.Reset();
    staging_.Reset();
    lz4_format_warned_ = false;
    refresh_hz_.store(new_hz, std::memory_order_relaxed);
    format_.store(new_format, std::memory_order_relaxed);
    height_.store(new_height, std::memory_order_relaxed);
    width_.store(new_width, std::memory_order_release);

    if (old_w != 0) {
        KRG_LOG("display mode changed (%ux%u @%uHz %s -> %ux%u @%uHz %s), renegotiating with the "
                "client",
                old_w, old_h, old_hz, format_name(old_format), new_width, new_height, new_hz,
                format_name(new_format));
        mode_changed_.store(true, std::memory_order_release);
    }
}

void DxgiCapture::release_duplication() {
    if (!dup_) return;
    dup_.Reset();
    // A mode change that lands while we are not looking goes unnoticed until
    // the duplication comes back; reinit_duplication() picks it up then.
    first_frame_ = true;
    // Capture is about to park until the next client, and however long that is
    // it is not part of any window worth averaging over. Zeroing the clock too
    // is what restarts the window from the first acquire rather than reporting
    // a handful of frames spread across the wait.
    stats_ = Stats{};
    stat_t0_ = {};
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
    const uint32_t w = width(), h = height();
    if (first_frame_ || info.TotalMetadataBufferSize == 0) {
        if (first_frame_) rects.push_back({0, 0, w, h});
        return;
    }

    metadata_.resize(info.TotalMetadataBufferSize);

    UINT move_bytes = 0;
    HRESULT hr = dup_->GetFrameMoveRects(static_cast<UINT>(metadata_.size()),
                                         reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadata_.data()),
                                         &move_bytes);
    if (FAILED(hr)) {
        rects.assign(1, {0, 0, w, h}); // can't trust metadata: resend everything
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
        rects.assign(1, {0, 0, w, h});
        return;
    }
    auto* dirty = reinterpret_cast<RECT*>(metadata_.data());
    for (UINT i = 0; i < dirty_bytes / sizeof(RECT); ++i) rects.push_back(rect_from(dirty[i]));
}

void DxgiCapture::report_stats() {
    const auto now = std::chrono::steady_clock::now();
    if (stat_t0_.time_since_epoch().count() == 0) {
        stat_t0_ = now;
        return;
    }
    if (now - stat_t0_ < std::chrono::seconds(5)) return;
    const double secs = std::chrono::duration<double>(now - stat_t0_).count();
    stat_t0_ = now;

    // Frames per second is the number that matters — everything downstream is
    // paced off it — so it leads, and the rest is why it is what it is. The
    // recoveries are the loud case: each one costs the 100 ms sleep below, so
    // even a handful per window caps capture far under the display's rate.
    KRG_LOG("capture %.1f fps; %.0f/s idle polls, %.1f/s pointer-only", stats_.frames / secs,
            stats_.timeouts / secs, stats_.cursor_only / secs);
    const uint64_t recoveries = stats_.access_lost + stats_.invalid_call + stats_.errors;
    if (recoveries) {
        KRG_LOG("capture lost the duplication %llu times (%llu access lost, %llu invalid call, "
                "%llu other), costing at least %.1f s of this %.0f s window",
                recoveries, stats_.access_lost, stats_.invalid_call, stats_.errors,
                recoveries * 0.1, secs);
    }
    stats_ = Stats{};
}

bool DxgiCapture::acquire(ComPtr<ID3D11Texture2D>& acquired, bool& have_rects,
                          std::vector<Rect>& rects) {
    have_rects = false;
    report_stats();
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
        ++stats_.timeouts;
        Sleep(2); // static screen — normal, not an error
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        // ACCESS_LOST: mode switch, secure desktop, etc. INVALID_CALL: the
        // duplication is poisoned — seen when a game engages exclusive
        // fullscreen, especially on hybrid-GPU machines. Recreate it; the
        // next successful frame is a full-frame keyframe either way.
        //
        // Counted separately because they say different things about a stream
        // that has gone slow: a run of these is capture being knocked over
        // faster than it can stand back up, and the 100 ms below is then the
        // frame rate, not the network.
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            ++stats_.access_lost;
        } else {
            ++stats_.invalid_call;
        }
        Sleep(100);
        reinit_duplication();
        return false;
    }
    if (FAILED(hr)) {
        ++stats_.errors;
        KRG_LOG("AcquireNextFrame failed (hr=0x%08lX)", hr);
        Sleep(100);
        return false;
    }

    resource.As(&acquired);

    // The desktop image is whatever the desktop is in, and CopyResource between
    // mismatched formats is not an error — it is a silent no-op, which on an
    // HDR desktop means streaming a texture nothing ever wrote to. So the
    // acquired texture, not the mode description, decides what everything
    // downstream is cut to.
    D3D11_TEXTURE2D_DESC acquired_desc{};
    if (acquired) {
        acquired->GetDesc(&acquired_desc);
        adopt_mode(acquired_desc.Width, acquired_desc.Height,
                   refresh_hz_.load(std::memory_order_relaxed), acquired_desc.Format);
    }

    update_cursor(info);

    rects.clear();
    collect_rects(info, rects);

    // Only the mouse pointer changed: no pixels to send. The cursor state
    // recorded above still reaches the client as a cursor packet.
    if (rects.empty()) {
        ++stats_.cursor_only;
        dup_->ReleaseFrame();
        return false;
    }

    ++stats_.frames;
    have_rects = true;
    return true;
}

bool DxgiCapture::next_frame_texture(ComPtr<ID3D11Texture2D>& out) {
    ComPtr<ID3D11Texture2D> acquired;
    bool have_rects = false;
    std::vector<Rect> rects;
    if (!acquire(acquired, have_rects, rects)) return false;

    // Recreated here rather than at init so a mode change can simply drop them.
    if (!pool_[0]) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width();
        td.Height = height();
        td.MipLevels = 1;
        td.ArraySize = 1;
        // Matching what was acquired rather than assuming BGRA8, because
        // CopyResource below requires it. Asking DXGI to convert is what makes
        // this BGRA8 in practice; on a machine where that did not happen, the
        // copy at least still copies, and the encoder is left to report a
        // format it cannot take when it is built.
        td.Format = format();
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

    const uint32_t width_now = width(), height_now = height();
    // Everything below reads the desktop back as 4-byte BGRA and the wire
    // format says the same, so an HDR desktop is not something this path can
    // carry — and half-reading a half-float surface would put convincing
    // garbage on screen rather than failing. The video path handles it.
    if (format() != DXGI_FORMAT_B8G8R8A8_UNORM) {
        if (!lz4_format_warned_) {
            lz4_format_warned_ = true;
            KRG_LOG("desktop is %s; --codec lz4 carries BGRA8 only, so nothing will be sent. "
                    "Use the video path, or turn HDR off for this display.",
                    format_name(format()));
        }
        dup_->ReleaseFrame();
        return false;
    }
    if (!staging_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width_now;
        td.Height = height_now;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT thr = device_->CreateTexture2D(&td, nullptr, &staging_);
        if (FAILED(thr)) {
            KRG_LOG("staging texture creation failed (hr=0x%08lX)", thr);
            dup_->ReleaseFrame();
            return false;
        }
    }

    context_->CopyResource(staging_.Get(), acquired.Get());
    dup_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE map{};
    HRESULT hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) {
        KRG_LOG("staging map failed (hr=0x%08lX)", hr);
        return false;
    }
    out.width = width_now;
    out.height = height_now;
    out.pixels.resize(size_t{width_now} * height_now * 4);
    const auto* src = static_cast<const uint8_t*>(map.pData);
    for (uint32_t y = 0; y < height_now; ++y) {
        std::memcpy(out.pixels.data() + size_t{y} * width_now * 4,
                    src + size_t{y} * map.RowPitch, size_t{width_now} * 4);
    }
    context_->Unmap(staging_.Get(), 0);

    out.rects = std::move(rects);
    out.id = ++frame_id_;
    first_frame_ = false;
    return true;
}

} // namespace krg
