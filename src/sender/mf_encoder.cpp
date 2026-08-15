#include <initguid.h>

#include "sender/mf_encoder.h"

#include <strmif.h>

#include <codecapi.h>
#include <mferror.h>
#include <oleauto.h>

#include <algorithm>
#include <iterator>

#include "common/clock.h"
#include "common/log.h"
#include "common/protocol.h"

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

namespace {

// Profiles to try, best first. High profile's 8x8 transform and CABAC tuning
// are worth a lot on desktop content (text especially) and every hardware
// encoder made this century supports it, but Main is the safe floor.
struct Profile {
    UINT32 value;
    const char* name;
};
constexpr Profile kH264Profiles[] = {{eAVEncH264VProfile_High, "High"},
                                     {eAVEncH264VProfile_Main, "Main"}};
constexpr Profile kHevcProfiles[] = {{eAVEncH265VProfile_Main_420_8, "Main"}};

const char* codec_name(uint32_t codec) { return codec == kCodecHevc ? "HEVC" : "H.264"; }

// Stands in for "never, unless asked". A real infinite (0xFFFFFFFF) trips
// validation in some encoders; this is ~414 days at 60 fps, which is the same
// thing for a screen-sharing session and stays inside a signed 32-bit range.
constexpr uint32_t kInfiniteGop = 0x7FFFFFFF;

} // namespace

std::unique_ptr<MfVideoEncoder> MfVideoEncoder::create(ComPtr<ID3D11Device> device,
                                                       const Config& config) {
    std::unique_ptr<MfVideoEncoder> enc(new MfVideoEncoder());
    if (!enc->init(std::move(device), config)) return nullptr;
    return enc;
}

MfVideoEncoder::~MfVideoEncoder() {
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

bool MfVideoEncoder::init(ComPtr<ID3D11Device> device, const Config& config) {
    device_ = std::move(device);
    device_->GetImmediateContext(&context_);
    fps_ = config.fps;

    // MF worker threads touch the device concurrently with ours.
    ComPtr<ID3D10Multithread> mt;
    context_.As(&mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // ok if already initialized
    KRG_HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    UINT reset_token = 0;
    KRG_HR(MFCreateDXGIDeviceManager(&reset_token, &manager_));
    KRG_HR(manager_->ResetDevice(device_.Get(), reset_token));

    if (!init_video_processor(config.width, config.height, config.fps, config.input_format)) {
        return false;
    }

    // HEVC is worth roughly a third of the bitrate at equal quality, but not
    // every GPU encodes it and not every receiver decodes it, so it stays a
    // request rather than a requirement. A machine can also enumerate an HEVC
    // encoder that then refuses the format, which is why the fallback covers
    // the whole setup rather than just the lookup.
    if (config.codec == kCodecHevc && !setup_transform(config, kCodecHevc)) {
        KRG_LOG("no usable HEVC encoder here, falling back to H.264");
        release_transform();
    }
    if (!transform_ && !setup_transform(config, kCodecH264)) return false;

    // NV12 sample pool shared with the MFT via the DXGI manager.
    KRG_HR(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_)));
    KRG_HR(allocator_->SetDirectXManager(manager_.Get()));
    ComPtr<IMFAttributes> alloc_attrs;
    KRG_HR(MFCreateAttributes(&alloc_attrs, 2));
    KRG_HR(alloc_attrs->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET));
    KRG_HR(alloc_attrs->SetUINT32(MF_SA_BUFFERS_PER_SAMPLE, 1));
    KRG_HR(allocator_->InitializeSampleAllocatorEx(2, 8, alloc_attrs.Get(), in_type_.Get()));

    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    KRG_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

    event_thread_ = std::thread([this] { event_loop(); });
    return true;
}

void MfVideoEncoder::release_transform() {
    events_.Reset();
    transform_.Reset();
    in_type_.Reset();
    codec_ = 0;
}

bool MfVideoEncoder::setup_transform(const Config& config, uint32_t codec) {
    if (!select_transform(codec)) return false;

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

    // Keyframe spacing has to be settled before the output type carries it.
    gop_ = config.gop ? config.gop : kInfiniteGop;
    configure_codec(config);
    // Encoders require the output type before the input type.
    if (!set_output_type(config)) return false;

    KRG_HR(MFCreateMediaType(&in_type_));
    KRG_HR(in_type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    KRG_HR(in_type_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    KRG_HR(MFSetAttributeSize(in_type_.Get(), MF_MT_FRAME_SIZE, config.width, config.height));
    KRG_HR(MFSetAttributeRatio(in_type_.Get(), MF_MT_FRAME_RATE, config.fps, 1));
    KRG_HR(MFSetAttributeRatio(in_type_.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    KRG_HR(in_type_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    KRG_HR(transform_->SetInputType(in_stream_, in_type_.Get(), 0));
    return true;
}

bool MfVideoEncoder::select_transform(uint32_t codec) {
    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video,
                                    codec == kCodecHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in_info,
                           &out_info, &activates, &count);
    if (FAILED(hr) || count == 0) {
        KRG_LOG("no hardware %s encoder found on this machine", codec_name(codec));
        if (SUCCEEDED(hr)) CoTaskMemFree(activates);
        return false;
    }
    // Activation can fail even for an enumerated MFT (stale GPU driver is the
    // usual cause), and multi-GPU machines list several — try each in order.
    for (UINT32 i = 0; i < count && !transform_; ++i) {
        WCHAR name[256] = L"?";
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);
        HRESULT activate_hr = activates[i]->ActivateObject(IID_PPV_ARGS(&transform_));
        if (SUCCEEDED(activate_hr)) {
            KRG_LOG("encoder: %ls (%s)", name, codec_name(codec));
        } else {
            KRG_LOG("encoder '%ls' failed to activate (hr=0x%08lX), trying next", name,
                    activate_hr);
        }
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (!transform_) {
        KRG_LOG("no hardware %s encoder could be activated — a GPU driver update usually "
                "fixes this", codec_name(codec));
        return false;
    }
    codec_ = codec;
    return true;
}

bool MfVideoEncoder::set_output_type(const Config& config) {
    const bool hevc = codec_ == kCodecHevc;
    const Profile* profiles = hevc ? kHevcProfiles : kH264Profiles;
    const size_t profile_count = hevc ? std::size(kHevcProfiles) : std::size(kH264Profiles);

    for (size_t i = 0; i < profile_count; ++i) {
        ComPtr<IMFMediaType> out_type;
        KRG_HR(MFCreateMediaType(&out_type));
        KRG_HR(out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        KRG_HR(out_type->SetGUID(MF_MT_SUBTYPE,
                                 hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264));
        KRG_HR(out_type->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate_bps));
        KRG_HR(MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height));
        KRG_HR(MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, config.fps, 1));
        KRG_HR(MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
        KRG_HR(out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        KRG_HR(out_type->SetUINT32(MF_MT_MPEG2_PROFILE, profiles[i].value));
        // Keyframe spacing belongs on the media type, not to ICodecAPI: the
        // NVIDIA MFT accepts CODECAPI_AVEncMPVGOPSize and then quietly keeps
        // its own default, whereas this it honours.
        KRG_HR(out_type->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, gop_));

        HRESULT hr = transform_->SetOutputType(out_stream_, out_type.Get(), 0);
        if (FAILED(hr)) {
            // Some encoders validate the spacing and reject an absurd one; a
            // shorter GOP is better than no stream.
            out_type->DeleteItem(MF_MT_MAX_KEYFRAME_SPACING);
            hr = transform_->SetOutputType(out_stream_, out_type.Get(), 0);
            if (SUCCEEDED(hr)) {
                KRG_LOG("encoder: keyframe spacing %u rejected, using the MFT's own", gop_);
                gop_ = 0;
            }
        }
        if (SUCCEEDED(hr)) {
            KRG_LOG("encoder: %s %s profile, %u Mbit/s target at %u fps", codec_name(codec_),
                    profiles[i].name, config.bitrate_bps / 1'000'000, config.fps);
            return true;
        }
        KRG_LOG("encoder: %s profile rejected (hr=0x%08lX)%s", profiles[i].name, hr,
                i + 1 < profile_count ? ", trying the next one down" : "");
    }
    return false;
}

// Must run *before* SetOutputType. The NVIDIA MFT accepts these settings
// afterwards too — SetValue returns S_OK and GetValue even reads the value
// back — and then encodes with its defaults anyway, which is how a GOP request
// of two billion frames turns into an IDR every sixty.
void MfVideoEncoder::configure_codec(const Config& config) {
    // Low-latency knobs; not every vendor MFT supports every one, so failures
    // are logged but not fatal.
    ComPtr<ICodecAPI> codec;
    if (FAILED(transform_.As(&codec))) {
        KRG_LOG("encoder: no ICodecAPI, running with the MFT's defaults");
        return;
    }

    VARIANT v{};
    auto set_u32 = [&](const GUID& guid, UINT32 value) {
        v.vt = VT_UI4;
        v.ulVal = value;
        return SUCCEEDED(codec->SetValue(&guid, &v));
    };
    auto set_bool = [&](const GUID& guid, bool value) {
        v.vt = VT_BOOL;
        v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        return SUCCEEDED(codec->SetValue(&guid, &v));
    };
    auto require = [](bool ok, const char* what) {
        if (!ok) KRG_LOG("encoder: %s unsupported", what);
        return ok;
    };

    require(set_u32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR), "CBR");
    require(set_u32(CODECAPI_AVEncCommonMeanBitRate, config.bitrate_bps), "mean bitrate");
    require(set_u32(CODECAPI_AVEncMPVDefaultBPictureCount, 0), "zero B-frames");
    require(set_bool(CODECAPI_AVEncCommonRealTime, true), "realtime mode");
    // Whether this one takes decides whether the MFT holds frames internally,
    // which the run loop otherwise has to shake loose by re-submitting. The
    // 5-second stats line reports the depth actually observed.
    if (require(set_bool(CODECAPI_AVLowLatencyMode, true), "low latency mode")) {
        KRG_LOG("encoder: low latency mode accepted");
    }

    // The media type carries the authoritative keyframe spacing (see
    // set_output_type); this is the same request through the other door, for
    // MFTs that read it here instead. Neither is reliable enough alone.
    if (gop_) set_u32(CODECAPI_AVEncMPVGOPSize, gop_);
    if (!gop_) {
        KRG_LOG("encoder: keyframes at the MFT's own interval");
    } else if (gop_ >= kInfiniteGop) {
        KRG_LOG("encoder: GOP effectively infinite, keyframes on demand only");
    } else {
        KRG_LOG("encoder: GOP %u frames (%.0f s)", gop_, double(gop_) / config.fps);
    }
}

bool MfVideoEncoder::init_video_processor(uint32_t width, uint32_t height, uint32_t fps,
                                          DXGI_FORMAT input_format) {
    if (FAILED(device_.As(&video_device_)) || FAILED(context_.As(&video_context_))) {
        KRG_LOG("device has no video support (conversion to NV12 unavailable)");
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

    // Asked rather than assumed. Capture hands over BGRA8 — it asks DXGI to
    // convert an HDR desktop down rather than pass the float surface along —
    // so anything else here means that conversion did not happen, and this is
    // where it can be said out loud. Left to itself the failure is per frame
    // and silent, inside CreateVideoProcessorInputView, and the stream is a
    // texture nothing ever wrote to. (Not a theoretical fallback: an RTX 4090's
    // video processor refuses R16G16B16A16_FLOAT as an input format outright.)
    UINT support = 0;
    if (FAILED(vp_enum_->CheckVideoProcessorFormat(input_format, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        KRG_LOG("the video processor will not take DXGI format %d as input, so frames in it "
                "cannot be converted for the encoder%s", static_cast<int>(input_format),
                input_format == DXGI_FORMAT_R16G16B16A16_FLOAT
                    ? " — this is an HDR desktop DXGI declined to convert; turning HDR off for "
                      "this display will stream it"
                    : "");
        return false;
    }
    KRG_HR(video_device_->CreateVideoProcessor(vp_enum_.Get(), 0, &vp_));

    // Full-range RGB in, BT.709 limited-range NV12 out (what decoders and the
    // receiver's shader expect).
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

bool MfVideoEncoder::convert_to_nv12(ID3D11Texture2D* source, ComPtr<IMFSample>& out) {
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
    if (FAILED(video_device_->CreateVideoProcessorInputView(source, vp_enum_.Get(), &in_desc,
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

    // Real clock timestamps (100ns units, as MF wants) so the packet header can
    // carry the capture time end to end; encoders only need them monotonic.
    sample->SetSampleTime(now_us() * 10);
    sample->SetSampleDuration(10'000'000 / fps_);
    out = std::move(sample);
    return true;
}

bool MfVideoEncoder::encode(ID3D11Texture2D* source) {
    ComPtr<IMFSample> sample;
    if (!convert_to_nv12(source, sample)) {
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
        ++submitted_;
    } else {
        if (!pending_) ++queued_;
        pending_ = std::move(sample); // replace any queued frame: latest wins
    }
    return true;
}

int MfVideoEncoder::pipeline_depth() const {
    // Ordered so a concurrent handoff from pending_ to the MFT can round the
    // answer up but never down: a spurious re-submit costs one frame of
    // bandwidth, a missed one leaves the last frame stuck in the encoder.
    int queued = queued_.load(std::memory_order_relaxed);
    int emitted = emitted_.load(std::memory_order_relaxed);
    int submitted = submitted_.load(std::memory_order_relaxed);
    return submitted - emitted + queued;
}

// Both of the runtime knobs below are driven from the run loop while the event
// thread is inside ProcessInput/ProcessOutput, so they take the same lock those
// do rather than reaching into the MFT alongside them.

void MfVideoEncoder::request_keyframe() {
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform_.As(&codec))) {
        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = 1;
        std::lock_guard lock(mutex_);
        codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
    }
}

bool MfVideoEncoder::set_bitrate(uint32_t bitrate_bps) {
    ComPtr<ICodecAPI> codec;
    if (FAILED(transform_.As(&codec))) return false;

    VARIANT v{};
    v.vt = VT_UI4;
    v.ulVal = bitrate_bps;
    std::lock_guard lock(mutex_);
    if (FAILED(codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v))) {
        KRG_LOG("encoder: mean bitrate not settable mid-stream, rate stays at the initial target");
        return false;
    }

    // configure_codec's warning applies here too: a vendor MFT can accept a
    // setting after the output type is set and encode with its own anyway. A
    // readback that disagrees proves the change was dropped; one that agrees
    // proves nothing, which is why the 5-second stats line reports the wire
    // rate next to the target rather than trusting this.
    if (!bitrate_readback_checked_) {
        bitrate_readback_checked_ = true;
        VARIANT got{};
        if (SUCCEEDED(codec->GetValue(&CODECAPI_AVEncCommonMeanBitRate, &got))) {
            if (got.vt != VT_UI4 || got.ulVal != bitrate_bps) {
                KRG_LOG("encoder: bitrate change did not read back (asked %u, got %u) — this "
                        "encoder may ignore rate control; --no-adapt pins the rate instead",
                        bitrate_bps, got.vt == VT_UI4 ? got.ulVal : 0);
            }
            VariantClear(&got);
        }
    }
    return true;
}

void MfVideoEncoder::set_sink(Sink sink) {
    std::lock_guard lock(sink_mutex_);
    sink_ = std::move(sink);
}

void MfVideoEncoder::event_loop() {
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

void MfVideoEncoder::on_need_input() {
    std::lock_guard lock(mutex_);
    if (pending_) {
        HRESULT hr = transform_->ProcessInput(in_stream_, pending_.Get(), 0);
        if (FAILED(hr)) {
            KRG_LOG("ProcessInput failed (hr=0x%08lX)", hr);
        } else {
            ++submitted_;
        }
        pending_.Reset();
        --queued_;
    } else {
        ++input_credits_;
    }
}

void MfVideoEncoder::on_have_output() {
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
    ++emitted_;

    ComPtr<IMFSample> sample;
    sample.Attach(out.pSample);
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;

    // Capture-to-wire latency on the sender's own clock. It rides along in the
    // packet header so the receiver can show where the milliseconds went, and
    // it is elapsed time rather than an absolute, so no clock sync is needed.
    LONGLONG ts = 0;
    int64_t capture_us = SUCCEEDED(sample->GetSampleTime(&ts)) ? ts / 10 : 0;
    uint32_t encode_us =
        capture_us ? static_cast<uint32_t>(std::max<int64_t>(0, now_us() - capture_us)) : 0;

    BYTE* data = nullptr;
    DWORD max_len = 0, len = 0;
    if (FAILED(buffer->Lock(&data, &max_len, &len))) return;
    bool keyframe = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, 0) != 0;
    {
        std::lock_guard lock(sink_mutex_);
        if (sink_) sink_(data, len, keyframe, capture_us, encode_us);
    }
    buffer->Unlock();

    if (!capture_us) return;
    double ms = encode_us / 1000.0;
    latency_sum_ms_ += ms;
    if (ms > latency_max_ms_) latency_max_ms_ = ms;
    ++latency_count_;
    if (last_log_time_ == 0) last_log_time_ = now_us();
    if (now_us() - last_log_time_ >= 5'000'000) {
        last_log_time_ = now_us();
        // Pipeline depth is the vendor answer to "is low-latency mode real?":
        // 0 means output is 1:1 with input and no frame is ever stuck waiting
        // for its successor.
        KRG_LOG("encode latency avg %.0f ms, max %.0f ms; pipeline depth %d; %u frames dropped "
                "pre-encode",
                latency_sum_ms_ / latency_count_, latency_max_ms_, pipeline_depth(),
                dropped_.load());
        latency_sum_ms_ = 0;
        latency_max_ms_ = 0;
        latency_count_ = 0;
        dropped_ = 0;
    }
}

} // namespace krg
