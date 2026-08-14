#include <initguid.h>

#include "sender/mf_encoder.h"

#include <strmif.h>

#include <codecapi.h>
#include <mferror.h>

#include "common/log.h"

using Microsoft::WRL::ComPtr;

// Fail-fast helper for init paths: logs the failing call and returns false.
#define KRG_HR(expr)                                          \
    do {                                                      \
        HRESULT hr_ = (expr);                                 \
        if (FAILED(hr_)) {                                    \
            KRG_LOG("%s failed (hr=0x%08lX)", #expr, hr_);    \
            return false;                                     \
        }                                                     \
    } while (0)

namespace krg {

std::unique_ptr<MfH264Encoder> MfH264Encoder::create(ComPtr<ID3D11Device> device, uint32_t width,
                                                     uint32_t height, uint32_t fps,
                                                     uint32_t bitrate_bps) {
    std::unique_ptr<MfH264Encoder> enc(new MfH264Encoder());
    if (!enc->init(std::move(device), width, height, fps, bitrate_bps)) return nullptr;
    return enc;
}

MfH264Encoder::~MfH264Encoder() {
    stop_ = true;
    if (transform_) {
        std::lock_guard lock(mutex_);
        // Drain produces a METransformDrainComplete event, waking the event
        // thread so it can observe stop_.
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    if (event_thread_.joinable()) event_thread_.join();
}

bool MfH264Encoder::init(ComPtr<ID3D11Device> device, uint32_t width, uint32_t height,
                         uint32_t fps, uint32_t bitrate_bps) {
    device_ = std::move(device);
    device_->GetImmediateContext(&context_);
    fps_ = fps;

    // MF worker threads touch the device concurrently with ours.
    ComPtr<ID3D10Multithread> mt;
    context_.As(&mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // ok if already initialized
    KRG_HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    UINT reset_token = 0;
    KRG_HR(MFCreateDXGIDeviceManager(&reset_token, &manager_));
    KRG_HR(manager_->ResetDevice(device_.Get(), reset_token));

    if (!init_video_processor(width, height, fps)) return false;

    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    KRG_HR(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                     MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in_info, &out_info,
                     &activates, &count));
    if (count == 0) {
        KRG_LOG("no hardware H.264 encoder found on this machine");
        return false;
    }
    // Activation can fail even for an enumerated MFT (stale GPU driver is the
    // usual cause), and multi-GPU machines list several — try each in order.
    for (UINT32 i = 0; i < count && !transform_; ++i) {
        WCHAR name[256] = L"?";
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);
        HRESULT hr = activates[i]->ActivateObject(IID_PPV_ARGS(&transform_));
        if (SUCCEEDED(hr)) {
            KRG_LOG("encoder: %ls", name);
        } else {
            KRG_LOG("encoder '%ls' failed to activate (hr=0x%08lX), trying next", name, hr);
        }
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (!transform_) {
        KRG_LOG("no hardware H.264 encoder could be activated — a GPU driver update usually fixes this");
        return false;
    }

    ComPtr<IMFAttributes> attrs;
    KRG_HR(transform_->GetAttributes(&attrs));
    KRG_HR(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
    KRG_HR(transform_.As(&events_));

    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                      reinterpret_cast<ULONG_PTR>(manager_.Get())));

    DWORD in_min, in_max, out_min, out_max;
    if (SUCCEEDED(transform_->GetStreamLimits(&in_min, &in_max, &out_min, &out_max))) {
        DWORD in_ids[1], out_ids[1];
        if (SUCCEEDED(transform_->GetStreamIDs(1, in_ids, 1, out_ids))) {
            in_stream_ = in_ids[0];
            out_stream_ = out_ids[0];
        }
    }

    // Encoders require the output type first.
    ComPtr<IMFMediaType> out_type;
    KRG_HR(MFCreateMediaType(&out_type));
    KRG_HR(out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    KRG_HR(out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    KRG_HR(out_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps));
    KRG_HR(MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, width, height));
    KRG_HR(MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, fps, 1));
    KRG_HR(MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    KRG_HR(out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    KRG_HR(out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main));
    KRG_HR(transform_->SetOutputType(out_stream_, out_type.Get(), 0));

    ComPtr<IMFMediaType> in_type;
    KRG_HR(MFCreateMediaType(&in_type));
    KRG_HR(in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    KRG_HR(in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    KRG_HR(MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, width, height));
    KRG_HR(MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, fps, 1));
    KRG_HR(MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    KRG_HR(in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    KRG_HR(transform_->SetInputType(in_stream_, in_type.Get(), 0));

    // Low-latency knobs; not every vendor MFT supports every one, so failures
    // are logged but not fatal.
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform_.As(&codec))) {
        VARIANT v{};
        auto set_u32 = [&](const GUID& guid, UINT32 value, const char* what) {
            v.vt = VT_UI4;
            v.ulVal = value;
            if (FAILED(codec->SetValue(&guid, &v))) KRG_LOG("encoder: %s unsupported", what);
        };
        auto set_bool = [&](const GUID& guid, bool value, const char* what) {
            v.vt = VT_BOOL;
            v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
            if (FAILED(codec->SetValue(&guid, &v))) KRG_LOG("encoder: %s unsupported", what);
        };
        set_u32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "CBR");
        set_u32(CODECAPI_AVEncCommonMeanBitRate, bitrate_bps, "mean bitrate");
        set_bool(CODECAPI_AVLowLatencyMode, true, "low latency mode");
        set_bool(CODECAPI_AVEncCommonRealTime, true, "realtime mode");
        set_u32(CODECAPI_AVEncMPVDefaultBPictureCount, 0, "zero B-frames");
        set_u32(CODECAPI_AVEncMPVGOPSize, fps * 10, "GOP size");
    }

    // NV12 sample pool shared with the MFT via the DXGI manager.
    KRG_HR(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_)));
    KRG_HR(allocator_->SetDirectXManager(manager_.Get()));
    ComPtr<IMFAttributes> alloc_attrs;
    KRG_HR(MFCreateAttributes(&alloc_attrs, 2));
    KRG_HR(alloc_attrs->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET));
    KRG_HR(alloc_attrs->SetUINT32(MF_SA_BUFFERS_PER_SAMPLE, 1));
    KRG_HR(allocator_->InitializeSampleAllocatorEx(2, 8, alloc_attrs.Get(), in_type.Get()));

    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

    event_thread_ = std::thread([this] { event_loop(); });
    return true;
}

bool MfH264Encoder::init_video_processor(uint32_t width, uint32_t height, uint32_t fps) {
    if (FAILED(device_.As(&video_device_)) || FAILED(context_.As(&video_context_))) {
        KRG_LOG("device has no video support (BGRA->NV12 conversion unavailable)");
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate = {fps, 1};
    desc.InputWidth = width;
    desc.InputHeight = height;
    desc.OutputFrameRate = {fps, 1};
    desc.OutputWidth = width;
    desc.OutputHeight = height;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    KRG_HR(video_device_->CreateVideoProcessorEnumerator(&desc, &vp_enum_));
    KRG_HR(video_device_->CreateVideoProcessor(vp_enum_.Get(), 0, &vp_));

    // BGRA full-range in, BT.709 limited-range NV12 out (what decoders and
    // the receiver's shader expect).
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
    in_cs.RGB_Range = 0;      // full
    in_cs.YCbCr_Matrix = 1;   // BT.709
    video_context_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in_cs);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
    out_cs.YCbCr_Matrix = 1;
    out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    video_context_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out_cs);
    return true;
}

bool MfH264Encoder::convert_to_nv12(ID3D11Texture2D* bgra, ComPtr<IMFSample>& out) {
    ComPtr<IMFSample> sample;
    HRESULT hr = allocator_->AllocateSample(&sample);
    if (FAILED(hr)) return false; // pool exhausted: encoder backlogged, drop frame

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return false;
    ComPtr<IMFDXGIBuffer> dxgi;
    if (FAILED(buffer.As(&dxgi))) return false;
    ComPtr<ID3D11Texture2D> nv12;
    if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&nv12)))) return false;
    UINT subresource = 0;
    dxgi->GetSubresourceIndex(&subresource);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};
    in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> in_view;
    if (FAILED(video_device_->CreateVideoProcessorInputView(bgra, vp_enum_.Get(), &in_desc,
                                                            &in_view))) {
        return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};
    out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    out_desc.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorOutputView> out_view;
    if (FAILED(video_device_->CreateVideoProcessorOutputView(nv12.Get(), vp_enum_.Get(),
                                                             &out_desc, &out_view))) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in_view.Get();
    if (FAILED(video_context_->VideoProcessorBlt(vp_.Get(), out_view.Get(), 0, 1, &stream))) {
        return false;
    }

    // Real clock timestamps so on_have_output can measure capture-to-wire
    // latency; encoders only need monotonic times.
    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(10'000'000 / fps_);
    ++frame_index_;
    out = std::move(sample);
    return true;
}

bool MfH264Encoder::encode(ID3D11Texture2D* bgra) {
    ComPtr<IMFSample> sample;
    if (!convert_to_nv12(bgra, sample)) {
        ++dropped_; // encoder backlogged (sample pool exhausted); not fatal
        return true;
    }

    std::lock_guard lock(mutex_);
    if (input_credits_ > 0) {
        --input_credits_;
        HRESULT hr = transform_->ProcessInput(in_stream_, sample.Get(), 0);
        if (FAILED(hr)) {
            KRG_LOG("ProcessInput failed (hr=0x%08lX)", hr);
            return false;
        }
    } else {
        pending_ = std::move(sample); // replace any queued frame: latest wins
    }
    return true;
}

void MfH264Encoder::request_keyframe() {
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform_.As(&codec))) {
        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = 1;
        codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
    }
}

void MfH264Encoder::set_sink(Sink sink) {
    std::lock_guard lock(sink_mutex_);
    sink_ = std::move(sink);
}

void MfH264Encoder::event_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!stop_) {
        ComPtr<IMFMediaEvent> event;
        if (FAILED(events_->GetEvent(0, &event))) break;
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == METransformNeedInput) {
            on_need_input();
        } else if (type == METransformHaveOutput) {
            on_have_output();
        }
    }
    CoUninitialize();
}

void MfH264Encoder::on_need_input() {
    std::lock_guard lock(mutex_);
    if (pending_) {
        HRESULT hr = transform_->ProcessInput(in_stream_, pending_.Get(), 0);
        if (FAILED(hr)) KRG_LOG("ProcessInput failed (hr=0x%08lX)", hr);
        pending_.Reset();
    } else {
        ++input_credits_;
    }
}

void MfH264Encoder::on_have_output() {
    MFT_OUTPUT_DATA_BUFFER out{};
    out.dwStreamID = out_stream_;
    DWORD status = 0;
    HRESULT hr;
    {
        std::lock_guard lock(mutex_);
        hr = transform_->ProcessOutput(0, 1, &out, &status);
    }
    if (out.pEvents) out.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> type;
        std::lock_guard lock(mutex_);
        if (SUCCEEDED(transform_->GetOutputAvailableType(out_stream_, 0, &type))) {
            transform_->SetOutputType(out_stream_, type.Get(), 0);
        }
        return;
    }
    if (FAILED(hr) || !out.pSample) return;

    ComPtr<IMFSample> sample;
    sample.Attach(out.pSample);
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;

    BYTE* data = nullptr;
    DWORD max_len = 0, len = 0;
    if (FAILED(buffer->Lock(&data, &max_len, &len))) return;
    bool keyframe = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, 0) != 0;
    {
        std::lock_guard lock(sink_mutex_);
        if (sink_) sink_(data, len, keyframe);
    }
    buffer->Unlock();

    // Capture-to-wire latency on the sender's own clock; if the stream feels
    // delayed but this stays low, the delay lives in the network or receiver.
    LONGLONG ts = 0;
    if (SUCCEEDED(sample->GetSampleTime(&ts))) {
        double ms = (MFGetSystemTime() - ts) / 10'000.0;
        latency_sum_ms_ += ms;
        if (ms > latency_max_ms_) latency_max_ms_ = ms;
        ++latency_count_;
        if (last_log_time_ == 0) last_log_time_ = MFGetSystemTime();
        if (MFGetSystemTime() - last_log_time_ >= 5 * 10'000'000LL) {
            last_log_time_ = MFGetSystemTime();
            KRG_LOG("encode latency avg %.0f ms, max %.0f ms; %u frames dropped pre-encode",
                    latency_sum_ms_ / latency_count_, latency_max_ms_, dropped_.load());
            latency_sum_ms_ = 0;
            latency_max_ms_ = 0;
            latency_count_ = 0;
            dropped_ = 0;
        }
    }
}

} // namespace krg
