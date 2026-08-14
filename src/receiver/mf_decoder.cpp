#include <initguid.h>

#include "receiver/mf_decoder.h"

#include <strmif.h>

#include <codecapi.h>
#include <cstring>
#include <mferror.h>

#include "common/log.h"

using Microsoft::WRL::ComPtr;

#define KRG_HR(expr)                                          \
    do {                                                      \
        HRESULT hr_ = (expr);                                 \
        if (FAILED(hr_)) {                                    \
            KRG_LOG("%s failed (hr=0x%08lX)", #expr, hr_);    \
            return false;                                     \
        }                                                     \
    } while (0)

namespace krg {

std::unique_ptr<MfH264Decoder> MfH264Decoder::create(ComPtr<ID3D11Device> device, uint32_t width,
                                                     uint32_t height) {
    std::unique_ptr<MfH264Decoder> dec(new MfH264Decoder());
    if (!dec->init(std::move(device), width, height)) return nullptr;
    return dec;
}

bool MfH264Decoder::init(ComPtr<ID3D11Device> device, uint32_t width, uint32_t height) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // ok if already initialized
    KRG_HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    UINT reset_token = 0;
    KRG_HR(MFCreateDXGIDeviceManager(&reset_token, &manager_));
    KRG_HR(manager_->ResetDevice(device.Get(), reset_token));

    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_H264};
    MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    KRG_HR(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                     &in_info, &out_info, &activates, &count));
    if (count == 0) {
        KRG_LOG("no H.264 decoder found on this machine");
        return false;
    }
    HRESULT hr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform_));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    KRG_HR(hr);

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(transform_->GetAttributes(&attrs))) {
        UINT32 aware = 0;
        attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware);
        if (!aware) {
            KRG_LOG("H.264 decoder is not D3D11-aware; hardware decode unavailable");
            return false;
        }
    }

    ComPtr<IMFMediaType> in_type;
    KRG_HR(MFCreateMediaType(&in_type));
    KRG_HR(in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    KRG_HR(in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    KRG_HR(MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, width, height));
    KRG_HR(MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, fps_, 1));
    KRG_HR(transform_->SetInputType(0, in_type.Get(), 0));

    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                      reinterpret_cast<ULONG_PTR>(manager_.Get())));

    if (!negotiate_output_type()) return false;

    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform_.As(&codec))) {
        VARIANT v{};
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    return true;
}

bool MfH264Decoder::negotiate_output_type() {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = transform_->GetOutputAvailableType(0, i, &type);
        if (FAILED(hr)) break;
        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12) {
            KRG_HR(transform_->SetOutputType(0, type.Get(), 0));
            return true;
        }
    }
    KRG_LOG("decoder offered no NV12 output type");
    return false;
}

bool MfH264Decoder::decode(const uint8_t* data, size_t size, const FrameFn& on_frame) {
    ComPtr<IMFMediaBuffer> buffer;
    KRG_HR(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer));
    BYTE* dst = nullptr;
    KRG_HR(buffer->Lock(&dst, nullptr, nullptr));
    std::memcpy(dst, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));

    ComPtr<IMFSample> in_sample;
    KRG_HR(MFCreateSample(&in_sample));
    KRG_HR(in_sample->AddBuffer(buffer.Get()));
    const int64_t duration = 10'000'000 / fps_;
    in_sample->SetSampleTime(frame_index_ * duration);
    in_sample->SetSampleDuration(duration);
    ++frame_index_;

    HRESULT hr = transform_->ProcessInput(0, in_sample.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        KRG_LOG("decoder ProcessInput failed (hr=0x%08lX)", hr);
        return false;
    }

    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out{};
        DWORD status = 0;
        hr = transform_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!negotiate_output_type()) return false;
            continue;
        }
        if (FAILED(hr)) {
            KRG_LOG("decoder ProcessOutput failed (hr=0x%08lX)", hr);
            return false;
        }
        if (!out.pSample) continue;

        ComPtr<IMFSample> sample;
        sample.Attach(out.pSample);
        ComPtr<IMFMediaBuffer> out_buffer;
        if (SUCCEEDED(sample->GetBufferByIndex(0, &out_buffer))) {
            ComPtr<IMFDXGIBuffer> dxgi;
            if (SUCCEEDED(out_buffer.As(&dxgi))) {
                ComPtr<ID3D11Texture2D> texture;
                UINT subresource = 0;
                if (SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&texture)))) {
                    dxgi->GetSubresourceIndex(&subresource);
                    on_frame(texture.Get(), subresource);
                }
            }
        }
    }
}

} // namespace krg
