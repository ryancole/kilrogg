#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

namespace krg {

// Hardware video encoder (Media Foundation async MFT). encode() takes a BGRA
// D3D11 texture on the caller's thread, converts it to NV12 on the GPU, and
// queues it; encoded Annex B packets arrive on an internal event thread via
// the sink callback. If the encoder is backlogged, the queued input frame is
// replaced (latest wins) — stale frames are dropped before encoding, never
// after, since encoded frames depend on their predecessors.
class MfVideoEncoder {
public:
    struct Config {
        uint32_t width = 0, height = 0;
        uint32_t fps = 60;
        uint32_t bitrate_bps = 0;
        // kCodecH264 or kCodecHevc. HEVC falls back to H.264 if this machine
        // has no HEVC encoder; ask codec() for what was actually created.
        uint32_t codec = 1;
        // Frames between IDRs. 0 asks for the longest GOP the encoder will
        // accept — effectively infinite, leaving keyframes on-demand only.
        uint32_t gop = 0;
        // What encode() will be handed. Checked against what the GPU's video
        // processor will actually convert, so that a format it cannot take is
        // one error at startup rather than every frame silently vanishing.
        DXGI_FORMAT input_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    };

    // `capture_us` is the sender-clock time the frame was captured and
    // `encode_us` the elapsed capture-to-here time; see VideoPacketHeader.
    using Sink = std::function<void(const uint8_t* data, size_t size, bool keyframe,
                                    int64_t capture_us, uint32_t encode_us)>;

    static std::unique_ptr<MfVideoEncoder> create(Microsoft::WRL::ComPtr<ID3D11Device> device,
                                                  const Config& config);
    ~MfVideoEncoder();

    // Sink runs on the encoder thread; pass nullptr to drop output.
    void set_sink(Sink sink);
    // The texture must be in the Config's input_format; anything else fails the
    // conversion and the frame is dropped.
    bool encode(ID3D11Texture2D* source);
    void request_keyframe();
    // Retargets the CBR rate mid-stream; see rate_control.h for who asks and
    // why. False means the MFT refused, and the caller should stop trying.
    bool set_bitrate(uint32_t bitrate_bps);

    uint32_t codec() const { return codec_; }
    // The frame rate the encoder actually accepted, which is the Config's
    // unless that asked for more than the codec's levels allow at this size.
    // Feed it at this rate: it budgets bits per frame from this number.
    uint32_t fps() const { return fps_; }

    // Frames handed to the MFT that have not come back out yet, i.e. how deep
    // the encoder's own pipeline is. Zero means output is 1:1 with input and
    // nothing needs to be pushed through to flush a frame loose.
    int pipeline_depth() const;

private:
    MfVideoEncoder() = default;
    bool init(Microsoft::WRL::ComPtr<ID3D11Device> device, const Config& config);
    // Activates and fully configures an encoder for one codec, so that a
    // failure anywhere in the sequence can be undone and retried with another.
    bool setup_transform(const Config& config, uint32_t codec);
    void release_transform();
    bool select_transform(uint32_t codec);
    // Settles the output type, and with it the frame rate: set_output_type
    // works down a ladder of rates, each one a full pass over the profiles.
    bool set_output_type(const Config& config);
    bool try_output_type(const Config& config, uint32_t fps);
    void configure_codec(const Config& config);
    bool init_video_processor(uint32_t width, uint32_t height, uint32_t fps,
                              DXGI_FORMAT input_format);
    bool convert_to_nv12(ID3D11Texture2D* source, Microsoft::WRL::ComPtr<IMFSample>& out);
    void event_loop();
    void on_need_input();
    void on_have_output();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vp_enum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;
    Microsoft::WRL::ComPtr<IMFMediaType> in_type_; // kept for the sample allocator
    Microsoft::WRL::ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    DWORD in_stream_ = 0, out_stream_ = 0;
    uint32_t fps_ = 60;
    uint32_t codec_ = 0;
    uint32_t gop_ = 0; // keyframe spacing actually in force; 0 = the MFT's own

    std::mutex mutex_; // guards transform_ calls, credits, pending input
    bool bitrate_readback_checked_ = false; // guarded by mutex_
    int input_credits_ = 0;
    Microsoft::WRL::ComPtr<IMFSample> pending_;

    std::mutex sink_mutex_;
    Sink sink_;

    std::thread event_thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> dropped_{0};
    // Counted at ProcessInput/ProcessOutput; their difference is the MFT's
    // internal frame delay, which the run loop uses to decide whether a quiet
    // tick needs a re-submit to flush the last frame out.
    std::atomic<int> submitted_{0}, emitted_{0}, queued_{0};
    double latency_sum_ms_ = 0, latency_max_ms_ = 0; // event thread only
    uint32_t latency_count_ = 0;
    int64_t last_log_time_ = 0;
};

} // namespace krg
