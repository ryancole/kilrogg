#include <windows.h>

#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <lz4.h>

#include "common/gate.h"
#include "common/log.h"
#include "common/mailbox.h"
#include "common/net.h"
#include "common/protocol.h"
#include "sender/control_receiver.h"
#include "sender/displays.h"
#include "sender/dummy_source.h"
#include "sender/dxgi_capture.h"
#include "sender/mf_encoder.h"
#include "sender/packet_sender.h"
#include "sender/rate_control.h"
#include "sender/rate_memory.h"

using Microsoft::WRL::ComPtr;

namespace krg {
namespace {

struct Options {
    bool dummy = false;
    bool lz4 = false;
    uint16_t port = kDefaultPort;
    uint32_t bitrate_mbps = 40;     // ceiling, unless --no-adapt pins it
    uint32_t min_bitrate_mbps = 3;  // how ugly the picture may get before giving up
    bool adapt = true;
    uint32_t codec = kCodecH264;
    // Whether --codec was given. The default is a preference and may be
    // improved on; a codec that was asked for by name is an instruction.
    bool codec_explicit = false;
    uint32_t gop = 0; // 0 = as long as the encoder allows
    uint32_t fps = 0; // 0 = whatever the display is actually refreshing at
    std::string display;    // index or name; empty = the primary
    bool show_displays = false; // --list-displays: print the list and stop
    bool show_encoders = false; // --list-encoders: probe the encoders and stop
};

void configure_client_socket(SOCKET s) {
    net::set_low_latency(s);
    // A receiver that has not accepted a single byte for this long is wedged
    // or gone; either way the viewer is staring at a frame seconds old, so
    // dropping the connection and listening again beats blocking in send()
    // until TCP's multi-minute retransmission timeout expires.
    net::set_send_timeout(s, 5);
    // Keepalive covers the other half of the problem: an idle stream (static
    // screen) has nothing in flight for the retransmission timer to act on.
    net::enable_keepalive(s, 5000, 1000);
}

// The receiver speaks first, listing the codecs it can decode. A short timeout
// keeps a port scan or a half-open connection from wedging the accept loop;
// blocking is restored afterwards because the back-channel is legitimately
// silent for minutes at a time.
bool read_client_hello(SOCKET s, ClientHello& out) {
    net::set_recv_timeout(s, 5);
    bool ok = net::recv_all(s, &out, sizeof(out));
    net::set_recv_timeout(s, 0);
    if (!ok) {
        KRG_LOG("no hello from client within 5s, dropping connection");
        return false;
    }
    if (out.magic != kMagicClient) {
        KRG_LOG("bad hello from client (magic 0x%08X), dropping connection", out.magic);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Legacy KRG1 path: dirty rects + LZ4. Lossless; kept as a debug reference
// (--codec lz4) while the video path matures.
// ---------------------------------------------------------------------------

// Sends one frame as (FrameHeader, rect_count x (RectHeader + LZ4 payload)).
// Returns bytes written, or -1 on socket/compression failure.
int64_t send_frame(SOCKET s, const Frame& frame, const std::vector<Rect>& dirty) {
    std::vector<Rect> rects;
    rects.reserve(dirty.size());
    for (Rect r : dirty) {
        r = clamp_rect(r, frame.width, frame.height);
        if (r.w && r.h) rects.push_back(r);
    }
    if (rects.empty()) return 0;

    int64_t bytes = 0;
    FrameHeader fh{frame.id, static_cast<uint32_t>(rects.size())};
    if (!net::send_all(s, &fh, sizeof(fh))) return -1;
    bytes += sizeof(fh);

    std::vector<uint8_t> raw, comp;
    for (const Rect& r : rects) {
        raw.resize(size_t{r.w} * r.h * 4);
        for (uint32_t y = 0; y < r.h; ++y) {
            std::memcpy(raw.data() + size_t{y} * r.w * 4,
                        frame.pixels.data() + (size_t{r.y + y} * frame.width + r.x) * 4,
                        size_t{r.w} * 4);
        }
        comp.resize(static_cast<size_t>(LZ4_compressBound(static_cast<int>(raw.size()))));
        int n = LZ4_compress_default(reinterpret_cast<const char*>(raw.data()),
                                     reinterpret_cast<char*>(comp.data()),
                                     static_cast<int>(raw.size()), static_cast<int>(comp.size()));
        if (n <= 0) return -1;

        RectHeader rh{r.x, r.y, r.w, r.h, static_cast<uint32_t>(n), static_cast<uint32_t>(raw.size())};
        if (!net::send_all(s, &rh, sizeof(rh))) return -1;
        if (!net::send_all(s, comp.data(), static_cast<size_t>(n))) return -1;
        bytes += sizeof(rh) + n;
    }
    return bytes;
}

int run_lz4(FrameSource& source, SOCKET listener) {
    const uint32_t w = source.width(), h = source.height();

    // Latest-wins handoff: if the send thread is behind, the newer frame
    // absorbs the dropped frame's dirty rects (its pixels are already the
    // full image, so nothing on screen is lost). Accumulated rects overlap —
    // once they cover more pixels than the frame itself, one keyframe is
    // strictly cheaper than resending the overlap.
    Mailbox<Frame> mailbox([w, h](Frame& incoming, Frame& pending) {
        incoming.rects.insert(incoming.rects.end(), pending.rects.begin(), pending.rects.end());
        uint64_t area = 0;
        for (const Rect& r : incoming.rects) area += uint64_t{r.w} * r.h;
        if (area > uint64_t{w} * h) incoming.rects.assign(1, Rect{0, 0, w, h});
    });

    // Parked between clients; this path pays for a frame twice over — a GPU
    // copy and a full readback to system memory — so idling it matters more
    // here than on the video path.
    Gate capture_gate;

    std::thread capture([&] {
        for (;;) {
            capture_gate.wait();
            Frame f;
            if (source.next_frame(f)) mailbox.push(std::move(f));
        }
    });
    capture.detach();

    Frame last; // most recent complete frame, reused as the keyframe on (re)connect
    for (;;) {
        SOCKET client = net::accept_client(listener);
        if (client == INVALID_SOCKET) continue;
        configure_client_socket(client);
        ClientHello client_hello;
        if (!read_client_hello(client, client_hello)) {
            closesocket(client);
            continue;
        }
        KRG_LOG("client connected");

        Hello hello{kMagic, w, h, kCodecNone};
        if (!net::send_all(client, &hello, sizeof(hello))) {
            closesocket(client);
            continue;
        }
        capture_gate.set(true);

        bool need_keyframe = true;
        uint32_t stat_frames = 0;
        int64_t stat_bytes = 0;
        auto stat_t0 = std::chrono::steady_clock::now();

        for (;;) {
            if (!need_keyframe || last.pixels.empty()) {
                auto f = mailbox.pop();
                if (!f) break;
                last = std::move(*f);
            }
            std::vector<Rect> rects =
                need_keyframe ? std::vector<Rect>{{0, 0, w, h}} : last.rects;
            int64_t n = send_frame(client, last, rects);
            if (n < 0) break;
            need_keyframe = false;

            ++stat_frames;
            stat_bytes += n;
            auto now = std::chrono::steady_clock::now();
            if (now - stat_t0 >= std::chrono::seconds(5)) {
                double secs = std::chrono::duration<double>(now - stat_t0).count();
                KRG_LOG("%.1f fps, %.2f Mbit/s", stat_frames / secs,
                        stat_bytes * 8.0 / 1e6 / secs);
                stat_frames = 0;
                stat_bytes = 0;
                stat_t0 = now;
            }
        }
        capture_gate.set(false);
        closesocket(client);
        KRG_LOG("client disconnected");
    }
}

// ---------------------------------------------------------------------------
// KRG2 path: hardware H.264. Frames stay on the GPU from capture to encoder.
// ---------------------------------------------------------------------------

// Ceiling on the quiet-tick re-submits described in the run loop. The depth it
// clamps is a property of the encoder, not the content, so anything this large
// means the reading is wrong and duplicate frames should stop rather than run.
constexpr int kMaxFlushResubmits = 8;

// How often the rate controller looks at the send queue. Short enough that a
// link going bad is answered within a few frames, long enough that the sample
// is not one frame's worth of noise.
constexpr auto kControlInterval = std::chrono::milliseconds(200);

// Forcing IDRs back to back is how a struggling link stays struggling: an IDR
// is the largest packet the encoder emits, so answering every overflow with one
// immediately refills the queue that just overflowed. Requests arriving inside
// this window are held rather than discarded — a receiver that cannot decode
// still gets its keyframe, just not instantly.
constexpr auto kMinKeyframeInterval = std::chrono::milliseconds(500);

// The longest the run loop will sit waiting for a frame that may never come.
// It bounds how late the control and stats intervals above can run on a static
// screen; on a moving one the frame pacing expires long before it does.
constexpr auto kQuietPoll = std::chrono::milliseconds(16);

// take_stats() resets its counters, so the control interval has to be the only
// caller; the 5-second log line is assembled from these instead.
void accumulate(PacketSender::Stats& acc, const PacketSender::Stats& st) {
    acc.packets += st.packets;
    acc.bytes += st.bytes;
    acc.dropped_packets += st.dropped_packets;
    acc.dropped_bytes += st.dropped_bytes;
    acc.backlog_flushes += st.backlog_flushes;
    acc.peak_queued_bytes = std::max(acc.peak_queued_bytes, st.peak_queued_bytes);
    acc.queued_bytes = st.queued_bytes; // a depth, not a delta: latest wins
}

// Cursor packets are produced on the run loop's thread and video packets on
// the encoder's event thread; the send queue serializes the two, so neither
// needs a lock of its own.
void queue_cursor(PacketSender& sender, const CursorPos& pos, const CursorShape* shape) {
    CursorUpdate cu{};
    cu.x = pos.x;
    cu.y = pos.y;
    cu.visible = pos.visible ? 1 : 0;
    if (shape) {
        cu.has_shape = 1;
        cu.width = shape->width;
        cu.height = shape->height;
    }
    std::span head{reinterpret_cast<const uint8_t*>(&cu), sizeof(cu)};
    if (shape) {
        sender.send_cursor(head, shape->bgra, shape->invert);
    } else {
        sender.send_cursor(head, {}, {});
    }
}

int run_h264(const Options& opt, SOCKET listener) {
    ComPtr<ID3D11Device> device;
    std::unique_ptr<DxgiCapture> capture_source;
    std::unique_ptr<DummySource> dummy_source;

    if (opt.dummy) {
        dummy_source = std::make_unique<DummySource>(1280, 720);
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr,
                                     0, D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
            KRG_LOG("D3D11CreateDevice failed");
            return 1;
        }
        KRG_LOG("using dummy frame source (1280x720)");
    } else {
        capture_source = DxgiCapture::create(opt.display);
        if (!capture_source) return 1;
        device = capture_source->device();
    }

    // Announced here rather than where the listener was opened, so that a
    // sender which is not going to capture anything — a --display naming a
    // screen this machine does not have — never claims to be listening first.
    if (opt.adapt) {
        KRG_LOG("listening on port %u (%s preferred, %u Mbit/s falling back to %u as the link "
                "requires)", opt.port, opt.codec == kCodecHevc ? "hevc" : "h264",
                opt.bitrate_mbps, opt.min_bitrate_mbps);
    } else {
        KRG_LOG("listening on port %u (%s preferred, %u Mbit/s fixed)", opt.port,
                opt.codec == kCodecHevc ? "hevc" : "h264", opt.bitrate_mbps);
    }

    // Read per connection rather than once: the desktop can change mode while
    // the sender is running, and the dimensions are fixed for the life of a
    // connection but not for the life of the process.
    auto source_width = [&] {
        return capture_source ? capture_source->width() : dummy_source->width();
    };
    auto source_height = [&] {
        return capture_source ? capture_source->height() : dummy_source->height();
    };

    // Latest-wins with no merge: video frames are complete images, and stale
    // ones can simply vanish (drops happen before encoding, never after).
    Mailbox<ComPtr<ID3D11Texture2D>> mailbox;

    // Capturing costs the same GPU work whether or not anyone is watching, so
    // the capture thread is parked between clients. On an idle machine that is
    // the difference between copying a full desktop image every time something
    // on screen moves and doing nothing at all.
    Gate capture_gate;

    std::thread capture([&] {
        if (capture_source) {
            for (;;) {
                if (!capture_gate.is_set()) {
                    // Dropping the duplication is only safe from the thread
                    // that runs the acquire loop, which is this one.
                    capture_source->release_duplication();
                    capture_gate.wait();
                }
                ComPtr<ID3D11Texture2D> tex;
                if (capture_source->next_frame_texture(tex)) mailbox.push(std::move(tex));
            }
        } else {
            // Dummy source produces CPU frames; upload through a small pool.
            const uint32_t w = dummy_source->width(), h = dummy_source->height();
            ComPtr<ID3D11DeviceContext> ctx;
            device->GetImmediateContext(&ctx);
            ComPtr<ID3D11Texture2D> pool[4];
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            for (auto& t : pool) device->CreateTexture2D(&td, nullptr, &t);
            size_t i = 0;
            for (;;) {
                capture_gate.wait();
                Frame f;
                if (!dummy_source->next_frame(f)) continue;
                ComPtr<ID3D11Texture2D>& slot = pool[i++ % std::size(pool)];
                if (!slot) continue;
                ctx->UpdateSubresource(slot.Get(), 0, nullptr, f.pixels.data(), w * 4, 0);
                mailbox.push(slot);
            }
        }
    });
    capture.detach();

    RateMemory rate_memory;
    for (;;) {
        SOCKET client = net::accept_client(listener);
        if (client == INVALID_SOCKET) continue;
        configure_client_socket(client);
        ClientHello client_hello;
        if (!read_client_hello(client, client_hello)) {
            closesocket(client);
            continue;
        }
        const std::string peer = net::peer_address(client);
        KRG_LOG("client connected%s%s", peer.empty() ? "" : " from ", peer.c_str());

        // Whatever the duplication last reported. A mode change that landed
        // while capture was parked is not visible until it resumes below, in
        // which case this connection is dropped a frame or two in and the
        // receiver reconnects against the right numbers.
        const uint32_t w = source_width(), h = source_height();
        // Part of the mode just as much as the dimensions are, and settled here
        // for the same reason: the encoder's conversion is built around it.
        const DXGI_FORMAT format =
            capture_source ? capture_source->format() : DXGI_FORMAT_B8G8R8A8_UNORM;
        // The encoder budgets its bits per frame from the frame rate it is
        // configured with, and nothing upstream limits how fast frames arrive:
        // capture polls with a zero timeout, so a 165 Hz desktop produces 165
        // frames a second. Told 60 and fed 165, the encoder spends most of
        // three times the commanded bitrate and the only thing that would
        // notice is a link slow enough to back up over it. So the rate is read
        // from the display, and submissions below are paced to it.
        // The encoder may not take this rate — H.264's levels run out somewhere
        // above 129 fps at 2560x1600 — so it is a request, and what the encoder
        // settled on is read back below.
        const uint32_t display_fps =
            opt.fps ? opt.fps : (capture_source ? capture_source->refresh_hz() : 60);

        const uint32_t ceiling_bps = opt.bitrate_mbps * 1'000'000;
        // --no-adapt makes the rate the operator's decision rather than the
        // link's, so nothing measured about the link applies to it.
        const uint32_t start_bps = opt.adapt ? rate_memory.recall(peer, ceiling_bps) : 0;
        RateControl rate(ceiling_bps, opt.min_bitrate_mbps * 1'000'000, start_bps);
        if (rate.target_bps() < ceiling_bps) {
            KRG_LOG("starting at %.1f Mbit/s, where this client's link left off",
                    rate.target_bps() / 1e6);
        }

        // Budget the backlog in time rather than bytes; it is retuned from the
        // control interval below whenever the encoder's rate changes.
        size_t queue_capacity = queue_budget_bytes(rate.target_bps());
        PacketSender sender(client, queue_capacity);

        // Both ends have to agree, and either may be the one that cannot do
        // HEVC — this machine's GPU or the receiver's decoder. Built fresh per
        // connection so the choice (and the encoder's whole state) follows the
        // client that is actually attached.
        MfVideoEncoder::Config cfg;
        cfg.width = w;
        cfg.height = h;
        cfg.fps = display_fps;
        cfg.bitrate_bps = rate.target_bps();
        // --no-adapt never retargets the encoder, so that one is built for
        // exactly the rate it was given and nothing above it.
        cfg.max_bitrate_bps = opt.adapt ? headroom_bps(ceiling_bps) : 0;
        cfg.gop = opt.gop;
        cfg.codec = opt.codec;
        cfg.input_format = format;
        if (cfg.codec == kCodecHevc && !(client_hello.codecs & kCodecHevc)) {
            KRG_LOG("receiver cannot decode HEVC, using H.264");
            cfg.codec = kCodecH264;
        }
        auto encoder = MfVideoEncoder::create(device, cfg);
        // One client's worth of bad luck — a mode change caught mid-create, or
        // another process holding the encoder — is not a reason to take the
        // sender down with it. A machine with no hardware encoder at all says
        // so once per connection attempt, which is diagnosis enough.
        if (!encoder) {
            KRG_LOG("could not create a hardware encoder, dropping connection");
            continue;
        }

        // H.264's levels are a macroblock-per-second budget and the encoders
        // built into GPUs stop at 5.2, so a large display at a high refresh
        // rate can ask for a level nobody offers. The encoder's whole reply is
        // a refused media type, and the ladder in set_output_type answers by
        // walking the frame rate down: 2560x1600 at 240 Hz settles on 129 fps,
        // which is felt as judder on a 240 Hz panel. HEVC budgets luma samples
        // instead and has room to spare at any rate a desktop refreshes at.
        //
        // Which encoders actually run out is not something to work out from
        // the spec. NVENC takes 3440x1440 at 175 fps, well past the 107 that
        // level 5.2 nominally allows, so a ceiling computed up front would
        // switch codecs on machines that never needed it. What the encoder
        // settled on is the only honest signal, so the upgrade is a second
        // attempt rather than a prediction — paid for only on the connections
        // that were going to judder anyway.
        if (!opt.codec_explicit && encoder->codec() == kCodecH264 &&
            encoder->fps() < display_fps && (client_hello.codecs & kCodecHevc)) {
            const uint32_t h264_fps = encoder->fps();
            KRG_LOG("H.264 would only take %ux%u at %u fps, short of this display's %u; trying "
                    "HEVC, whose levels have the headroom", w, h, h264_fps, display_fps);
            // Released before the second attempt rather than held alongside
            // it. An encoder occupies one of the card's session slots, and
            // running out of those is exactly how the sender ends up unable to
            // build an encoder at all.
            encoder.reset();
            MfVideoEncoder::Config hevc_cfg = cfg;
            hevc_cfg.codec = kCodecHevc;
            auto hevc = MfVideoEncoder::create(device, hevc_cfg);
            if (hevc && hevc->fps() > h264_fps) {
                encoder = std::move(hevc);
            } else {
                // Either HEVC would not build here or it bought no frames;
                // H.264 is the better answer in both cases, being the one
                // every receiver can decode.
                hevc.reset();
                KRG_LOG("HEVC is no better on this machine, staying with H.264 at %u fps",
                        h264_fps);
                encoder = MfVideoEncoder::create(device, cfg);
                if (!encoder) {
                    KRG_LOG("could not rebuild the H.264 encoder, dropping connection");
                    continue;
                }
            }
        }

        if (!(client_hello.codecs & encoder->codec())) {
            KRG_LOG("receiver decodes none of the codecs this machine can encode, "
                    "dropping connection");
            continue;
        }

        // Pacing, the bit budget and the stats all run on the rate frames are
        // actually submitted at, which is the encoder's answer rather than the
        // display's — commanding a 240 Hz budget into an encoder configured for
        // 129 would overspend the link by the ratio between them.
        const uint32_t fps = encoder->fps();
        const auto frame_interval = std::chrono::microseconds(1'000'000 / fps);

        Hello hello{kMagicVideo, w, h, encoder->codec()};
        if (!net::send_all(client, &hello, sizeof(hello))) continue;

        // Everything that could fail is behind us, so capture can start paying
        // for itself. The mailbox may still hold a frame from the last client
        // (or from before a mode change), which is neither current nor
        // necessarily the right size.
        mailbox.clear();
        capture_gate.set(true);

        // Constructed after the PacketSender so it is torn down first, while
        // the socket it reads from is still open.
        ControlReceiver control(client, sender);

        bool rate_control_live = opt.adapt;
        bool renegotiate = false;
        auto stat_t0 = std::chrono::steady_clock::now();
        auto ctl_t0 = stat_t0;
        PacketSender::Stats stat_acc;

        // The encoder divides the rate it is given by the frame rate it was
        // configured with; `budget` works out what to give it so that what
        // comes out is the rate the link was asked for. `submitted` is the
        // measurement it runs on, counted where frames actually go in.
        FrameBudget budget(fps);
        uint32_t commanded_bps = rate.target_bps(); // what the encoder was started at
        uint32_t submitted = 0;

        encoder->set_sink([&sender](const uint8_t* data, size_t size, bool keyframe,
                                    int64_t capture_us, uint32_t encode_us) {
            sender.send_video(data, size, keyframe, capture_us, encode_us);
        });
        encoder->request_keyframe();
        auto last_keyframe = std::chrono::steady_clock::now();
        bool keyframe_pending = false;

        // Async encoders can hold a pipeline of frames, emitting frame N only
        // once frame N+1 arrives; sparse content (a desktop with occasional
        // changes) would then sit in the encoder indefinitely. On a quiet tick,
        // re-submit the last frame to push the stuck ones out — but only as
        // many times as the encoder is actually holding, so a static screen
        // stops producing traffic and an encoder that honours low-latency mode
        // (depth 0) never pays for this at all.
        ComPtr<ID3D11Texture2D> last_tex;
        bool tex_unsent = false;
        int flush_budget = 0;
        auto next_submit = std::chrono::steady_clock::now();
        // Version 0 = "never sent to this client": the first poll always
        // delivers the current shape and position to a fresh connection.
        uint64_t cursor_pos_ver = 0, cursor_shape_ver = 0;
        CursorPos cursor_pos;
        while (!sender.dead() && !control.closed()) {
            auto now = std::chrono::steady_clock::now();

            // The desktop changed mode under us. Width and height are settled
            // at the handshake and fixed for the life of a connection, so a
            // new connection is how the receiver learns the new ones — it
            // reconnects on its own, and gets a correct Hello when it does.
            if (capture_source && capture_source->take_mode_change()) {
                renegotiate = true;
                break;
            }

            // Two ways to end up needing an IDR: the send queue threw away a
            // backlog, or the receiver failed to decode and said so. With a
            // GOP this long, asking is the only way one ever arrives.
            if (sender.take_resync_request() || control.take_keyframe_request()) {
                keyframe_pending = true;
            }
            if (keyframe_pending && now - last_keyframe >= kMinKeyframeInterval) {
                encoder->request_keyframe();
                keyframe_pending = false;
                last_keyframe = now;
            }

            if (now - ctl_t0 >= kControlInterval) {
                double secs = std::chrono::duration<double>(now - ctl_t0).count();
                ctl_t0 = now;
                // Counted at the socket, not at the encoder: under congestion
                // this is the rate the receiver actually sees, which is exactly
                // what the controller needs and what the 5-second line reports.
                PacketSender::Stats st = sender.take_stats();
                accumulate(stat_acc, st);

                if (rate_control_live) {
                    RateControl::Decision d = rate.update(
                        {st.bytes, st.backlog_flushes, st.queued_bytes, queue_capacity, secs});
                    if (d.bps) {
                        // The budget is a latency bound, so it follows the wire
                        // rate — which is the target, not the command below:
                        // the correction buys frames, not bandwidth.
                        queue_capacity = queue_budget_bytes(d.bps);
                        sender.set_max_queued_bytes(queue_capacity);
                        // Cuts are events worth seeing the moment they happen;
                        // the climb back is a line a second, so it is left to
                        // the 5-second line to report where it got to.
                        if (d.congested) {
                            KRG_LOG("link behind, dropping to %.1f Mbit/s (measured %.1f Mbit/s "
                                    "on the wire)",
                                    d.bps / 1e6, st.bytes * 8.0 / 1e6 / secs);
                        }
                    }
                    // What the encoder has to be told to actually put the
                    // target on the wire, given how many frames it was fed.
                    const uint32_t want = budget.command(rate.target_bps(), submitted, secs);
                    // An encoder that refuses the change leaves the rate where
                    // it is, so the queue keeps the budget that matches it and
                    // the controller stops being consulted at all.
                    if (want != commanded_bps) {
                        if (encoder->set_bitrate(want)) {
                            commanded_bps = want;
                        } else {
                            rate_control_live = false;
                        }
                    }
                }
                submitted = 0;
            }

            if (now - stat_t0 >= std::chrono::seconds(5)) {
                double secs = std::chrono::duration<double>(now - stat_t0).count();
                stat_t0 = now;
                const PacketSender::Stats& st = stat_acc;
                KRG_LOG("wire %.1f fps, %.2f Mbit/s (target %.1f); queue peak %zu KB, now %zu KB",
                        st.packets / secs, st.bytes * 8.0 / 1e6 / secs, rate.target_bps() / 1e6,
                        st.peak_queued_bytes >> 10, st.queued_bytes >> 10);
                // Only worth a line while it is doing something: a multiplier
                // of 1 means content is keeping up with the display, which is
                // the case this correction does not apply to.
                if (budget.multiplier() > 1.01) {
                    KRG_LOG("content is running at %.0f%% of %u Hz, so the encoder is commanded "
                            "%.1f Mbit/s to spend %.1f on the wire",
                            100.0 / budget.multiplier(), fps, commanded_bps / 1e6,
                            rate.target_bps() / 1e6);
                }
                if (st.backlog_flushes) {
                    // The bottleneck is whichever of the two is slower: the
                    // link, or a receiver that cannot decode and present as
                    // fast as this machine encodes.
                    KRG_LOG("link or receiver behind: %llu backlog flushes, %llu packets "
                            "(%.2f MB) dropped%s",
                            st.backlog_flushes, st.dropped_packets, st.dropped_bytes / 1e6,
                            rate_control_live ? "" : "; try a lower --bitrate if this persists");
                }
                stat_acc = PacketSender::Stats{};
            }

            if (capture_source) {
                CursorShape shape;
                bool shape_changed = capture_source->poll_cursor_shape(cursor_shape_ver, shape);
                bool pos_changed = capture_source->poll_cursor_pos(cursor_pos_ver, cursor_pos);
                if (shape_changed || pos_changed) {
                    queue_cursor(sender, cursor_pos, shape_changed ? &shape : nullptr);
                }
            }
            // Holding a frame that arrived early is what paces the encoder to
            // the rate it was configured for: newer frames replace it in the
            // mailbox while it waits, so what goes in at the slot is the
            // latest one either way. With nothing in hand there is no slot to
            // wait for, only the poll — which also keeps a static screen from
            // spinning here once the flush budget is spent.
            std::chrono::steady_clock::duration wait = kQuietPoll;
            if (tex_unsent && next_submit > now) {
                wait = std::min<std::chrono::steady_clock::duration>(kQuietPoll, next_submit - now);
            }
            auto tex = mailbox.pop_for(wait);
            if (tex) {
                // Capture may have adopted a new display mode between the
                // handshake and now; the flag that ends this connection is
                // checked once per iteration, so a frame cut to the new mode
                // can arrive first. The encoder is built for the old one —
                // including its pixel format, which changes on its own when
                // HDR is switched on or off without the size moving at all.
                D3D11_TEXTURE2D_DESC td{};
                (*tex)->GetDesc(&td);
                if (td.Width != w || td.Height != h || td.Format != format) continue;
                // Sampled before submitting: this is how many earlier frames
                // the MFT is still sitting on, and therefore how many pushes
                // it takes to get the frame we are about to hand it back out.
                flush_budget = std::min(encoder->pipeline_depth(), kMaxFlushResubmits);
                last_tex = std::move(*tex);
                tex_unsent = true;
            }

            const auto at = std::chrono::steady_clock::now();
            if (at < next_submit) continue; // this frame's slot has not come round
            if (!tex_unsent) {
                if (flush_budget <= 0 || !last_tex) continue;
                --flush_budget;
            }
            tex_unsent = false;
            next_submit += frame_interval;
            // Falling a whole frame behind — a stalled encoder, a capture that
            // went away for a moment — is not a reason to then send a burst
            // catching up on frames nobody will ever see.
            if (next_submit < at) next_submit = at + frame_interval;
            if (!encoder->encode(last_tex.Get())) break;
            ++submitted;
        }
        // Nothing is going to consume frames until the next client arrives.
        capture_gate.set(false);
        // Blocks until any in-flight sink call returns, so the encoder's event
        // thread is done with `sender` before either goes out of scope below.
        encoder->set_sink(nullptr);
        // Only worth remembering if it was arrived at rather than commanded,
        // and only if the encoder was actually honouring it.
        if (rate_control_live) rate_memory.remember(peer, rate.target_bps());
        if (renegotiate) {
            KRG_LOG("client dropped to renegotiate at %ux%u; waiting for it to reconnect",
                    source_width(), source_height());
        } else {
            KRG_LOG("client disconnected");
        }
    }
}

int run(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dummy") == 0) {
            opt.dummy = true;
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            opt.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            ++i;
            if (std::strcmp(argv[i], "lz4") == 0) {
                opt.lz4 = true;
            } else if (std::strcmp(argv[i], "hevc") == 0) {
                opt.codec = kCodecHevc;
            } else if (std::strcmp(argv[i], "h264") != 0) {
                KRG_LOG("unknown codec '%s' (h264|hevc|lz4)", argv[i]);
                return 2;
            }
            opt.codec_explicit = true;
        } else if (std::strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            opt.bitrate_mbps = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--min-bitrate") == 0 && i + 1 < argc) {
            opt.min_bitrate_mbps = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-adapt") == 0) {
            opt.adapt = false;
        } else if (std::strcmp(argv[i], "--gop") == 0 && i + 1 < argc) {
            opt.gop = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            opt.fps = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            opt.display = argv[++i];
        } else if (std::strcmp(argv[i], "--list-displays") == 0) {
            opt.show_displays = true;
        } else if (std::strcmp(argv[i], "--list-encoders") == 0) {
            opt.show_encoders = true;
        } else {
            KRG_LOG("usage: kilrogg-send [--dummy] [--port N] [--codec h264|hevc|lz4] "
                    "[--bitrate Mbps] [--min-bitrate Mbps] [--no-adapt] [--gop frames] "
                    "[--fps N] [--display N|name] [--list-displays] [--list-encoders]");
            return 2;
        }
    }
    // Before anything asks Windows about a display: the desktop coordinates
    // DXGI reports back are virtualized for a process that has not said this,
    // so a scaled monitor would list — and capture — at the size the scaling
    // pretends it is rather than the size it has.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (opt.show_displays) {
        print_displays(enumerate_displays());
        return 0;
    }
    if (opt.show_encoders) {
        print_encoders();
        return 0;
    }
    // A zero ceiling would leave the controller nothing to work with, and a
    // floor above the ceiling is a typo rather than a request.
    opt.bitrate_mbps = std::max(1u, opt.bitrate_mbps);
    opt.min_bitrate_mbps = std::clamp(opt.min_bitrate_mbps, 1u, opt.bitrate_mbps);
    // 0 keeps the default, which is to ask the display. Anything else is taken
    // as meant, within the range a display could plausibly be running at — the
    // upper bound is there because this divides the second up.
    if (opt.fps) opt.fps = std::clamp(opt.fps, 1u, 480u);

    timeBeginPeriod(1); // 1ms timer resolution: capture polling and frame
                        // pacing rely on short sleeps being actually short
    if (!net::init()) {
        KRG_LOG("WSAStartup failed");
        return 1;
    }

    SOCKET listener = net::listen_on(opt.port);
    if (listener == INVALID_SOCKET) return 1;

    if (opt.lz4) {
        std::unique_ptr<FrameSource> source;
        if (opt.dummy) {
            source = std::make_unique<DummySource>(1280, 720);
            KRG_LOG("using dummy frame source (1280x720)");
        } else {
            source = DxgiCapture::create(opt.display);
            if (!source) return 1;
        }
        KRG_LOG("listening on port %u, sharing %ux%u (lz4)", opt.port, source->width(),
                source->height());
        return run_lz4(*source, listener);
    }

    return run_h264(opt, listener);
}

} // namespace
} // namespace krg

int main(int argc, char** argv) {
    return krg::run(argc, argv);
}
