#pragma once
#include <memory>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "sender/frame_source.h"

namespace krg {

// Desktop Duplication capture of the primary output. next_frame() returning
// false is the common case on a static screen (AcquireNextFrame times out
// rather than erroring) — callers just loop.
class DxgiCapture final : public FrameSource {
public:
    static std::unique_ptr<DxgiCapture> create(); // null on failure

    bool next_frame(Frame& out) override;
    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }

private:
    DxgiCapture() = default;
    bool init();
    bool reinit_duplication();
    void collect_rects(const DXGI_OUTDUPL_FRAME_INFO& info, std::vector<Rect>& rects);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    std::vector<uint8_t> metadata_;
    uint32_t width_ = 0, height_ = 0;
    uint32_t frame_id_ = 0;
    bool first_frame_ = true;
};

} // namespace krg
