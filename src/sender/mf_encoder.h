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

// Hardware H.264 encoder (Media Foundation async MFT). encode() takes a BGRA
// D3D11 texture on the caller's thread, converts it to NV12 on the GPU, and
// queues it; encoded Annex B packets arrive on an internal event thread via
// the sink callback. If the encoder is backlogged, the queued input frame is
// replaced (latest wins) — stale frames are dropped before encoding, never
// after, since encoded frames depend on their predecessors.
class MfH264Encoder {
public:
    using Sink = std::function<void(const uint8_t* data, size_t size, bool keyframe)>;

    static std::unique_ptr<MfH264Encoder> create(Microsoft::WRL::ComPtr<ID3D11Device> device,
                                                 uint32_t width, uint32_t height, uint32_t fps,
                                                 uint32_t bitrate_bps);
    ~MfH264Encoder();

    // Sink runs on the encoder thread; pass nullptr to drop output.
    void set_sink(Sink sink);
    bool encode(ID3D11Texture2D* bgra);
    void request_keyframe();

private:
    MfH264Encoder() = default;
    bool init(Microsoft::WRL::ComPtr<ID3D11Device> device, uint32_t width, uint32_t height,
              uint32_t fps, uint32_t bitrate_bps);
    bool init_video_processor(uint32_t width, uint32_t height, uint32_t fps);
    bool convert_to_nv12(ID3D11Texture2D* bgra, Microsoft::WRL::ComPtr<IMFSample>& out);
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
    Microsoft::WRL::ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    DWORD in_stream_ = 0, out_stream_ = 0;
    uint32_t fps_ = 60;
    int64_t frame_index_ = 0;

    std::mutex mutex_; // guards transform_ calls, credits, pending input
    int input_credits_ = 0;
    Microsoft::WRL::ComPtr<IMFSample> pending_;

    std::mutex sink_mutex_;
    Sink sink_;

    std::thread event_thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> dropped_{0};
    double latency_sum_ms_ = 0, latency_max_ms_ = 0; // event thread only
    uint32_t latency_count_ = 0;
    int64_t last_log_time_ = 0;
};

} // namespace krg
