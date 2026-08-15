#include <windows.h>

#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <lz4.h>

#include "common/log.h"
#include "common/mailbox.h"
#include "common/net.h"
#include "common/protocol.h"
#include "sender/control_receiver.h"
#include "sender/dummy_source.h"
#include "sender/dxgi_capture.h"
#include "sender/mf_encoder.h"
#include "sender/packet_sender.h"
#include "sender/rate_control.h"

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
    uint32_t gop = 0; // 0 = as long as the encoder allows
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

Rect clamp_rect(Rect r, uint32_t w, uint32_t h) {
    if (r.x >= w || r.y >= h) return {0, 0, 0, 0};
    if (r.x + r.w > w) r.w = w - r.x;
    if (r.y + r.h > h) r.h = h - r.y;
    return r;
}

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

    std::thread capture([&] {
        for (;;) {
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

// The send queue's backlog budget: a quarter second of video at `bps`, which
// rides out a Wi-Fi retransmit burst without letting lag build to where it is
// felt. The floor keeps the queue larger than a single frame at absurdly low
// bitrates.
size_t queue_budget_bytes(uint32_t bps) {
    return std::max<size_t>(512u << 10, size_t{bps} / 32);
}

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
    uint32_t w = 0, h = 0;

    if (opt.dummy) {
        dummy_source = std::make_unique<DummySource>(1280, 720);
        w = dummy_source->width();
        h = dummy_source->height();
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr,
                                     0, D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
            KRG_LOG("D3D11CreateDevice failed");
            return 1;
        }
        KRG_LOG("using dummy frame source (1280x720)");
    } else {
        capture_source = DxgiCapture::create();
        if (!capture_source) return 1;
        device = capture_source->device();
        w = capture_source->width();
        h = capture_source->height();
    }

    // Latest-wins with no merge: video frames are complete images, and stale
    // ones can simply vanish (drops happen before encoding, never after).
    Mailbox<ComPtr<ID3D11Texture2D>> mailbox;

    std::thread capture([&] {
        if (capture_source) {
            for (;;) {
                ComPtr<ID3D11Texture2D> tex;
                if (capture_source->next_frame_texture(tex)) mailbox.push(std::move(tex));
            }
        } else {
            // Dummy source produces CPU frames; upload through a small pool.
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

        // Budget the backlog in time rather than bytes; it is retuned from the
        // control interval below whenever the encoder's rate changes.
        const uint32_t ceiling_bps = opt.bitrate_mbps * 1'000'000;
        size_t queue_capacity = queue_budget_bytes(ceiling_bps);
        PacketSender sender(client, queue_capacity);

        // Both ends have to agree, and either may be the one that cannot do
        // HEVC — this machine's GPU or the receiver's decoder. Built fresh per
        // connection so the choice (and the encoder's whole state) follows the
        // client that is actually attached.
        MfVideoEncoder::Config cfg;
        cfg.width = w;
        cfg.height = h;
        cfg.fps = 60;
        cfg.bitrate_bps = ceiling_bps;
        cfg.gop = opt.gop;
        cfg.codec = opt.codec;
        if (cfg.codec == kCodecHevc && !(client_hello.codecs & kCodecHevc)) {
            KRG_LOG("receiver cannot decode HEVC, using H.264");
            cfg.codec = kCodecH264;
        }
        auto encoder = MfVideoEncoder::create(device, cfg);
        if (!encoder) return 1;
        if (!(client_hello.codecs & encoder->codec())) {
            KRG_LOG("receiver decodes none of the codecs this machine can encode, "
                    "dropping connection");
            continue;
        }

        Hello hello{kMagicVideo, w, h, encoder->codec()};
        if (!net::send_all(client, &hello, sizeof(hello))) continue;

        // Constructed after the PacketSender so it is torn down first, while
        // the socket it reads from is still open.
        ControlReceiver control(client, sender);

        // Rebuilt per connection, so each client starts optimistic and learns
        // its own link rather than inheriting the last one's verdict.
        RateControl rate(ceiling_bps, opt.min_bitrate_mbps * 1'000'000);
        bool rate_control_live = opt.adapt;
        auto stat_t0 = std::chrono::steady_clock::now();
        auto ctl_t0 = stat_t0;
        PacketSender::Stats stat_acc;

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
        int flush_budget = 0;
        // Version 0 = "never sent to this client": the first poll always
        // delivers the current shape and position to a fresh connection.
        uint64_t cursor_pos_ver = 0, cursor_shape_ver = 0;
        CursorPos cursor_pos;
        while (!sender.dead() && !control.closed()) {
            auto now = std::chrono::steady_clock::now();

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
                    // An encoder that refuses the change leaves the rate where
                    // it is, so the queue keeps the budget that matches it and
                    // the controller stops being consulted at all.
                    if (d.bps && !encoder->set_bitrate(d.bps)) {
                        rate_control_live = false;
                    } else if (d.bps) {
                        // The budget is a latency bound, so it follows the rate.
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
                }
            }

            if (now - stat_t0 >= std::chrono::seconds(5)) {
                double secs = std::chrono::duration<double>(now - stat_t0).count();
                stat_t0 = now;
                const PacketSender::Stats& st = stat_acc;
                KRG_LOG("wire %.1f fps, %.2f Mbit/s (target %.1f); queue peak %zu KB, now %zu KB",
                        st.packets / secs, st.bytes * 8.0 / 1e6 / secs, rate.target_bps() / 1e6,
                        st.peak_queued_bytes >> 10, st.queued_bytes >> 10);
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
            auto tex = mailbox.pop_for(std::chrono::milliseconds(16));
            if (tex) {
                // Sampled before submitting: this is how many earlier frames
                // the MFT is still sitting on, and therefore how many pushes
                // it takes to get the frame we are about to hand it back out.
                flush_budget = std::min(encoder->pipeline_depth(), kMaxFlushResubmits);
                last_tex = std::move(*tex);
            } else {
                if (flush_budget <= 0 || !last_tex) continue;
                --flush_budget;
            }
            if (!encoder->encode(last_tex.Get())) break;
        }
        // Blocks until any in-flight sink call returns, so the encoder's event
        // thread is done with `sender` before either goes out of scope below.
        encoder->set_sink(nullptr);
        KRG_LOG("client disconnected");
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
        } else if (std::strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            opt.bitrate_mbps = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--min-bitrate") == 0 && i + 1 < argc) {
            opt.min_bitrate_mbps = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-adapt") == 0) {
            opt.adapt = false;
        } else if (std::strcmp(argv[i], "--gop") == 0 && i + 1 < argc) {
            opt.gop = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else {
            KRG_LOG("usage: kilrogg-send [--dummy] [--port N] [--codec h264|hevc|lz4] "
                    "[--bitrate Mbps] [--min-bitrate Mbps] [--no-adapt] [--gop frames]");
            return 2;
        }
    }
    // A zero ceiling would leave the controller nothing to work with, and a
    // floor above the ceiling is a typo rather than a request.
    opt.bitrate_mbps = std::max(1u, opt.bitrate_mbps);
    opt.min_bitrate_mbps = std::clamp(opt.min_bitrate_mbps, 1u, opt.bitrate_mbps);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
            source = DxgiCapture::create();
            if (!source) return 1;
        }
        KRG_LOG("listening on port %u, sharing %ux%u (lz4)", opt.port, source->width(),
                source->height());
        return run_lz4(*source, listener);
    }

    if (opt.adapt) {
        KRG_LOG("listening on port %u (%s preferred, %u Mbit/s falling back to %u as the link "
                "requires)", opt.port, opt.codec == kCodecHevc ? "hevc" : "h264",
                opt.bitrate_mbps, opt.min_bitrate_mbps);
    } else {
        KRG_LOG("listening on port %u (%s preferred, %u Mbit/s fixed)", opt.port,
                opt.codec == kCodecHevc ? "hevc" : "h264", opt.bitrate_mbps);
    }
    return run_h264(opt, listener);
}

} // namespace
} // namespace krg

int main(int argc, char** argv) {
    return krg::run(argc, argv);
}
