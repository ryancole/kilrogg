#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

namespace krg {

// Writes what --list-encoders prints: every hardware encoder registered on
// this machine, the GPU each belongs to, and whether it will actually start
// right now. The last of those is the point — an encoder that enumerates and
// then refuses to activate is the failure worth diagnosing, and this runs it
// without needing a client to connect first, so the same probe can be taken
// with and without whatever is suspected of holding the card.
void print_encoders();

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
        // The most set_bitrate() will ever ask for. An encoder clamps a
        // mid-stream rate change to the rate it was built with, silently, so
        // the transform is built for this and stepped straight back down to
        // bitrate_bps — leaving every later change inside the clamp. 0 (or
        // anything below bitrate_bps) builds for the starting rate alone,
        // which is what a caller that never retargets should ask for.
        uint32_t max_bitrate_bps = 0;
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
    // why. Never goes above Config::max_bitrate_bps — the MFT was built for
    // that and would clamp anything past it. False means the MFT refused, and
    // the caller should stop trying.
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
    // Builds it with the headroom set_bitrate needs, and builds it again
    // without if this MFT proves it will not come back down.
    bool setup_transform(const Config& config, uint32_t codec);
    // One build attempt, at one rate: the rate the MFT will treat as its
    // ceiling for the rest of its life.
    bool build_transform(const Config& config, uint32_t codec, uint32_t bitrate_bps);
    void release_transform();
    bool select_transform(uint32_t codec);
    // Enumerates and starts the first hardware encoder that will run, either
    // from one GPU's encoders (`luid`) or from every one the machine has
    // (null). True with transform_ set.
    bool activate_transform(uint32_t codec, const LUID* luid);
    // Settles the output type, and with it the frame rate: set_output_type
    // works down a ladder of rates, each one a full pass over the profiles.
    bool set_output_type(const Config& config);
    bool try_output_type(const Config& config, uint32_t fps);
    void configure_codec(const Config& config);
    // Sets the mean bitrate and checks the MFT agrees it took. Callers hold
    // mutex_ (or run before the event thread exists).
    bool apply_bitrate(uint32_t bitrate_bps);
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
    // The activation object transform_ came out of, kept for as long as the
    // transform is, so the MFT can be shut down the documented way rather than
    // merely dropped. What that is worth is measured in release_transform().
    Microsoft::WRL::ComPtr<IMFActivate> activate_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;
    Microsoft::WRL::ComPtr<IMFMediaType> in_type_; // kept for the sample allocator
    Microsoft::WRL::ComPtr<IMFVideoSampleAllocatorEx> allocator_;

    // The conversion needs a view onto each end of it, and building the pair
    // fresh every frame measured 17.5 us on an RTX 4090 at 3440x1440 — small
    // against a 5.7 ms frame interval, but paid on the run loop's own thread,
    // for views onto textures that were the same handful every time: capture
    // cycles through a pool of four and the sample allocator through eight, so
    // the second frame onward is always asking for a view that was just built
    // and thrown away.
    //
    // Each entry holds the texture as well as the view, so a key can never be a
    // freed pointer that a later texture was allocated on top of. That pins the
    // pool textures for as long as the encoder lives, which costs nothing: a
    // display mode change is what drops that pool, and it rebuilds the encoder
    // in the same breath.
    template <typename View>
    class ViewCache {
    public:
        View* find(ID3D11Texture2D* texture) const {
            for (const Entry& e : entries_) {
                if (e.texture.Get() == texture) return e.view.Get();
            }
            return nullptr;
        }
        void add(ID3D11Texture2D* texture, Microsoft::WRL::ComPtr<View> view) {
            // The two pools above are the only sources, so passing this means
            // the assumption behind the cache is wrong on this machine. Start
            // over rather than grow: a cache that never hits is a leak.
            if (entries_.size() >= kMaxViews) entries_.clear();
            entries_.push_back({texture, std::move(view)});
        }

    private:
        struct Entry {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<View> view;
        };
        static constexpr size_t kMaxViews = 16;
        std::vector<Entry> entries_;
    };
    // Touched only from convert_to_nv12, i.e. only from the thread that calls
    // encode() — never from the event thread — so no lock.
    ViewCache<ID3D11VideoProcessorInputView> in_views_;
    ViewCache<ID3D11VideoProcessorOutputView> out_views_;
    DWORD in_stream_ = 0, out_stream_ = 0;
    uint32_t fps_ = 60;
    uint32_t codec_ = 0;
    uint32_t gop_ = 0; // keyframe spacing actually in force; 0 = the MFT's own
    // The rate the transform was built at, and so the most it will encode at
    // however high it is retargeted afterwards.
    uint32_t build_bitrate_ = 0;
    // False once the MFT has proved it does not take rate changes; set_bitrate
    // then refuses rather than commanding rates that do nothing. Touched from
    // init and from the caller's thread in set_bitrate, never the event thread.
    bool rate_changes_usable_ = true;

    std::mutex mutex_; // guards transform_ calls, credits, pending input
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
