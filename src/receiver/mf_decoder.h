#pragma once
#include <cstdint>
#include <functional>
#include <memory>

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

namespace krg {

// Hardware-accelerated H.264 decoder (Media Foundation sync MFT + DXVA).
// Shares the caller's D3D11 device so decoded NV12 textures can be consumed
// directly by the presenter. decode() may emit zero or more frames per packet.
class MfH264Decoder {
public:
    using FrameFn = std::function<void(ID3D11Texture2D* nv12, UINT subresource)>;

    static std::unique_ptr<MfH264Decoder> create(Microsoft::WRL::ComPtr<ID3D11Device> device,
                                                 uint32_t width, uint32_t height);

    bool decode(const uint8_t* data, size_t size, const FrameFn& on_frame);

private:
    MfH264Decoder() = default;
    bool init(Microsoft::WRL::ComPtr<ID3D11Device> device, uint32_t width, uint32_t height);
    bool negotiate_output_type();

    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    uint32_t fps_ = 60;
    int64_t frame_index_ = 0;
};

} // namespace krg
