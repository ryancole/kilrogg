#include <initguid.h>

#include "receiver/mf_decoder.h"

#include <strmif.h>

#include <codecapi.h>
#include <cstring>
#include <mferror.h>

#include "common/log.h"
#include "common/protocol.h"

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

namespace {

const GUID& subtype_for(uint32_t codec) {
    return codec == kCodecHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

const char* codec_name(uint32_t codec) { return codec == kCodecHevc ? "HEVC" : "H.264"; }

// Enumerates decoders for one codec. Software MFTs are deliberately included:
// a machine without a hardware HEVC block may still have the Store extension,
// and being able to decode slowly beats not connecting.
UINT32 count_decoders(uint32_t codec) {
    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, subtype_for(codec)};
    MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in_info, &out_info,
                         &activates, &count))) {
        return 0;
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    return count;
}

} // namespace

uint32_t MfVideoDecoder::decodable_codecs() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // ok if already initialized
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 0;
    uint32_t codecs = 0;
    if (count_decoders(kCodecH264)) codecs |= kCodecH264;
    if (count_decoders(kCodecHevc)) codecs |= kCodecHevc;
    return codecs;
}

std::unique_ptr<MfVideoDecoder> MfVideoDecoder::create(ComPtr<ID3D11Device> device, uint32_t width,
                                                       uint32_t height, uint32_t codec) {
    std::unique_ptr<MfVideoDecoder> dec(new MfVideoDecoder());
    if (!dec->init(std::move(device), width, height, codec)) return nullptr;
    return dec;
}

bool MfVideoDecoder::init(ComPtr<ID3D11Device> device, uint32_t width, uint32_t height,
                          uint32_t codec) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // ok if already initialized
    KRG_HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    UINT reset_token = 0;
    KRG_HR(MFCreateDXGIDeviceManager(&reset_token, &manager_));
    KRG_HR(manager_->ResetDevice(device.Get(), reset_token));

    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, subtype_for(codec)};
    MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    KRG_HR(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                     &in_info, &out_info, &activates, &count));
    if (count == 0) {
        KRG_LOG("no %s decoder found on this machine", codec_name(codec));
        CoTaskMemFree(activates);
        return false;
    }
    // Enumeration order is best-first, but activation can still fail (a stale
    // driver, a Store codec mid-update) — try each before giving up.
    for (UINT32 i = 0; i < count && !transform_; ++i) {
        WCHAR name[256] = L"?";
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);
        HRESULT hr = activates[i]->ActivateObject(IID_PPV_ARGS(&transform_));
        if (SUCCEEDED(hr)) {
            KRG_LOG("decoder: %ls (%s)", name, codec_name(codec));
        } else {
            KRG_LOG("decoder '%ls' failed to activate (hr=0x%08lX), trying next", name, hr);
        }
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (!transform_) {
        KRG_LOG("no %s decoder could be activated", codec_name(codec));
        return false;
    }

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(transform_->GetAttributes(&attrs))) {
        UINT32 aware = 0;
        attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware);
        if (!aware) {
            KRG_LOG("%s decoder is not D3D11-aware; hardware decode unavailable",
                    codec_name(codec));
            return false;
        }
    }

    ComPtr<IMFMediaType> in_type;
    KRG_HR(MFCreateMediaType(&in_type));
    KRG_HR(in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    KRG_HR(in_type->SetGUID(MF_MT_SUBTYPE, subtype_for(codec)));
    KRG_HR(MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, width, height));
    KRG_HR(MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, fps_, 1));
    KRG_HR(transform_->SetInputType(0, in_type.Get(), 0));

    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                      reinterpret_cast<ULONG_PTR>(manager_.Get())));

    if (!negotiate_output_type()) return false;

    ComPtr<ICodecAPI> codec_api;
    if (SUCCEEDED(transform_.As(&codec_api))) {
        VARIANT v{};
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    return true;
}

bool MfVideoDecoder::negotiate_output_type() {
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

void MfVideoDecoder::flush() {
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

bool MfVideoDecoder::decode(const uint8_t* data, size_t size, const FrameFn& on_frame) {
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
