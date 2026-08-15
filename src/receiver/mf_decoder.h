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

// Hardware-accelerated video decoder (Media Foundation sync MFT + DXVA).
// Shares the caller's D3D11 device so decoded NV12 textures can be consumed
// directly by the presenter. decode() may emit zero or more frames per packet.
class MfVideoDecoder {
public:
    using FrameFn = std::function<void(ID3D11Texture2D* nv12, UINT subresource)>;

    // Bitmask of kCodecH264 | kCodecHevc — what this machine has a decoder
    // for. Sent to the sender at connect so it can pick a codec both ends
    // speak; HEVC in particular is missing on plenty of otherwise fine
    // Windows installs.
    static uint32_t decodable_codecs();

    static std::unique_ptr<MfVideoDecoder> create(Microsoft::WRL::ComPtr<ID3D11Device> device,
                                                  uint32_t width, uint32_t height,
                                                  uint32_t codec);

    // False means the packet could not be decoded. The caller should ask the
    // sender for a keyframe rather than drop the connection: with a long GOP
    // that IDR is the only thing that will resynchronise the stream.
    bool decode(const uint8_t* data, size_t size, const FrameFn& on_frame);

    // Discards the reference frames the MFT is holding, so the garbage that
    // follows a failed packet is not blended into the frames after the IDR.
    void flush();

private:
    MfVideoDecoder() = default;
    bool init(Microsoft::WRL::ComPtr<ID3D11Device> device, uint32_t width, uint32_t height,
              uint32_t codec);
    bool negotiate_output_type();

    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    uint32_t fps_ = 60;
    int64_t frame_index_ = 0;
};

} // namespace krg
