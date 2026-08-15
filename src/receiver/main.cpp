#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <lz4.h>

#include "common/clock.h"
#include "common/log.h"
#include "common/mailbox.h"
#include "common/net.h"
#include "common/protocol.h"
#include "receiver/mf_decoder.h"
#include "receiver/presenter.h"

namespace krg {
namespace {

struct Options {
    std::string host;
    uint16_t port = kDefaultPort;
    bool waitable = false;
    bool stats = false;
};

// ---------------------------------------------------------------------------
// KRG1: LZ4 dirty rects.
// ---------------------------------------------------------------------------

struct UpdateBatch {
    uint32_t frame_id = 0;
    std::vector<RectUpdate> updates;
};

// Reads frames off the socket, decompresses each rect, and hands batches to
// the present loop. Returns when the connection drops or data is invalid.
void receive_loop_lz4(SOCKET s, uint32_t width, uint32_t height, Mailbox<UpdateBatch>& mailbox) {
    std::vector<uint8_t> comp;
    for (;;) {
        FrameHeader fh;
        if (!net::recv_all(s, &fh, sizeof(fh))) return;
        if (fh.rect_count > 4096) {
            KRG_LOG("bogus rect count %u, dropping connection", fh.rect_count);
            return;
        }

        UpdateBatch batch;
        batch.frame_id = fh.frame_id;
        batch.updates.reserve(fh.rect_count);
        for (uint32_t i = 0; i < fh.rect_count; ++i) {
            RectHeader rh;
            if (!net::recv_all(s, &rh, sizeof(rh))) return;
            bool valid = rh.w > 0 && rh.h > 0 && rh.x + rh.w <= width && rh.y + rh.h <= height &&
                         rh.raw_size == rh.w * rh.h * 4 && rh.comp_size > 0 &&
                         rh.comp_size <= (size_t{64} << 20);
            if (!valid) {
                KRG_LOG("invalid rect header, dropping connection");
                return;
            }

            comp.resize(rh.comp_size);
            if (!net::recv_all(s, comp.data(), comp.size())) return;

            RectUpdate u;
            u.rect = {rh.x, rh.y, rh.w, rh.h};
            u.pixels.resize(rh.raw_size);
            int n = LZ4_decompress_safe(reinterpret_cast<const char*>(comp.data()),
                                        reinterpret_cast<char*>(u.pixels.data()),
                                        static_cast<int>(comp.size()),
                                        static_cast<int>(u.pixels.size()));
            if (n != static_cast<int>(rh.raw_size)) {
                KRG_LOG("LZ4 decompression failed, dropping connection");
                return;
            }
            batch.updates.push_back(std::move(u));
        }
        mailbox.push(std::move(batch));
    }
}

int run_lz4(SOCKET s, const Hello& hello) {
    auto presenter = Presenter::create(hello.width, hello.height);
    if (!presenter) return 1;

    // Latest-wins with ordered merge: if the present loop is behind, prepend
    // the pending (older) batch so rects still apply oldest-first.
    Mailbox<UpdateBatch> mailbox([](UpdateBatch& incoming, UpdateBatch& pending) {
        incoming.updates.insert(incoming.updates.begin(),
                                std::make_move_iterator(pending.updates.begin()),
                                std::make_move_iterator(pending.updates.end()));
    });

    std::atomic<bool> net_dead{false};
    std::thread rx([&] {
        receive_loop_lz4(s, hello.width, hello.height, mailbox);
        net_dead = true;
        mailbox.stop();
    });

    uint32_t presented = 0;
    while (presenter->pump()) {
        if (net_dead) {
            KRG_LOG("connection closed");
            break;
        }
        auto batch = mailbox.pop_for(std::chrono::milliseconds(2));
        if (batch) {
            presenter->apply(batch->updates);
            presenter->present();
            if (++presented == 1) KRG_LOG("first frame presented");
        }
    }

    shutdown(s, SD_BOTH); // unblocks the recv thread without freeing the handle
    rx.join();
    closesocket(s);
    return 0;
}

// ---------------------------------------------------------------------------
// KRG3: hardware video.
// ---------------------------------------------------------------------------

// Serializes the back-channel: keyframe requests originate on the receive
// thread and clock probes on the present loop. Both messages are a few bytes,
// and the sender reads continuously, so a blocking send here cannot park for
// long enough to matter.
class ControlChannel {
public:
    explicit ControlChannel(SOCKET s) : socket_(s) {}

    void request_keyframe() { send(kControlKeyframe, nullptr, 0); }
    void ping(int64_t client_us) { send(kControlPing, &client_us, sizeof(client_us)); }

private:
    void send(uint32_t type, const void* payload, uint32_t size) {
        ControlHeader header{type, size};
        std::lock_guard lock(mutex_);
        if (!net::send_all(socket_, &header, sizeof(header))) return;
        if (size) net::send_all(socket_, payload, size);
    }

    SOCKET socket_;
    std::mutex mutex_;
};

// What one frame cost, stage by stage. Sender-clock and receiver-clock fields
// are kept apart deliberately: only the offset estimate below can bridge them.
struct FrameTiming {
    int64_t capture_us = 0;  // sender clock; 0 marks a cursor-only repaint
    uint32_t encode_us = 0;  // elapsed on the sender, so no clock skew
    int64_t arrival_us = 0;  // receiver clock, last byte off the socket
    int64_t decoded_us = 0;  // receiver clock, decoder handed back a texture
};

// The two machines' clocks share no epoch, so a sender timestamp means nothing
// locally until the offset between them is known. Ping/pong gives it: at the
// midpoint of a round trip both clocks are, by assumption, reading the same
// instant. The estimate keeps the sample with the shortest round trip, since
// that is the one least polluted by queueing in either direction.
class ClockSync {
public:
    void on_pong(const Pong& pong, int64_t now) {
        int64_t rtt = now - pong.client_us;
        if (rtt < 0 || rtt >= best_rtt_us_) return;
        best_rtt_us_ = rtt;
        rtt_us_.store(rtt, std::memory_order_relaxed);
        offset_us_.store(pong.server_us - (pong.client_us + rtt / 2), std::memory_order_relaxed);
        valid_.store(true, std::memory_order_relaxed);
    }

    bool valid() const { return valid_.load(std::memory_order_relaxed); }
    // Sender clock reading, in receiver-clock terms.
    int64_t to_local(int64_t sender_us) const {
        return sender_us - offset_us_.load(std::memory_order_relaxed);
    }
    double rtt_ms() const { return rtt_us_.load(std::memory_order_relaxed) / 1000.0; }

private:
    int64_t best_rtt_us_ = std::numeric_limits<int64_t>::max(); // receive thread only
    std::atomic<int64_t> offset_us_{0};
    std::atomic<int64_t> rtt_us_{0};
    std::atomic<bool> valid_{false};
};

// Averages each stage over a window and renders the overlay text. Everything
// here is touched only by the present loop except `bytes`, which the receive
// thread adds to.
class StatsWindow {
public:
    void add_bytes(size_t n) { bytes_.fetch_add(n, std::memory_order_relaxed); }

    void add_frame(const FrameTiming& t, int64_t present_us, const ClockSync& clock) {
        ++frames_;
        encode_ms_ += t.encode_us / 1000.0;
        decode_ms_ += (t.decoded_us - t.arrival_us) / 1000.0;
        queue_ms_ += (present_us - t.decoded_us) / 1000.0;
        if (clock.valid()) {
            network_ms_ += (t.arrival_us - clock.to_local(t.capture_us + t.encode_us)) / 1000.0;
            e2e_ms_ += (present_us - clock.to_local(t.capture_us)) / 1000.0;
            ++synced_frames_;
        }
    }

    // Formats and clears the window. `scanout_ms` is negative when DXGI has
    // nothing to report, which is the normal case for torn presents.
    std::string take_text(const char* codec, uint32_t w, uint32_t h, double scanout_ms,
                          const ClockSync& clock, int64_t elapsed_us) {
        const double secs = elapsed_us / 1e6;
        const double n = frames_ ? double(frames_) : 1.0;
        const double s = synced_frames_ ? double(synced_frames_) : 1.0;
        const uint64_t bytes = bytes_.exchange(0, std::memory_order_relaxed);

        char buf[512];
        char scanout[16] = "    --";
        if (scanout_ms >= 0) std::snprintf(scanout, sizeof(scanout), "%6.1f", scanout_ms);
        char network[16] = "    --", e2e[16] = "    --", rtt[16] = "--";
        if (synced_frames_) {
            std::snprintf(network, sizeof(network), "%6.1f", network_ms_ / s);
            std::snprintf(e2e, sizeof(e2e), "%6.1f", e2e_ms_ / s);
            std::snprintf(rtt, sizeof(rtt), "%.1f", clock.rtt_ms());
        }
        std::snprintf(buf, sizeof(buf),
                      "%s  %ux%u\n"
                      "%.1f fps   %.1f Mbit/s\n"
                      "\n"
                      "encode  %6.1f ms\n"
                      "network %s ms\n"
                      "decode  %6.1f ms\n"
                      "queue   %6.1f ms\n"
                      "scanout %s ms\n"
                      "\n"
                      "capture to present %s ms\n"
                      "link rtt %s ms",
                      codec, w, h, frames_ / secs, bytes * 8.0 / 1e6 / secs, encode_ms_ / n,
                      network, decode_ms_ / n, queue_ms_ / n, scanout, e2e, rtt);

        frames_ = synced_frames_ = 0;
        encode_ms_ = decode_ms_ = queue_ms_ = network_ms_ = e2e_ms_ = 0;
        return buf;
    }

private:
    std::atomic<uint64_t> bytes_{0};
    uint32_t frames_ = 0, synced_frames_ = 0;
    double encode_ms_ = 0, decode_ms_ = 0, queue_ms_ = 0, network_ms_ = 0, e2e_ms_ = 0;
};

int run_video(SOCKET s, const Hello& hello, const Options& opt) {
    Presenter::Options popt;
    popt.video_mode = true;
    popt.waitable = opt.waitable;
    popt.stats = opt.stats;
    auto presenter = Presenter::create(hello.width, hello.height, popt);
    if (!presenter) return 1;

    auto decoder =
        MfVideoDecoder::create(presenter->device(), hello.width, hello.height, hello.codec);
    if (!decoder) return 1;

    ControlChannel control(s);
    ClockSync clock;
    StatsWindow stats;
    Mailbox<FrameTiming> frame_ready;

    std::atomic<bool> net_dead{false};
    std::thread rx([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<uint8_t> payload;
        int64_t last_resync_us = 0;
        for (;;) {
            VideoPacketHeader ph;
            if (!net::recv_all(s, &ph, sizeof(ph))) break;
            if (ph.size == 0 || ph.size > (32u << 20)) {
                KRG_LOG("bogus packet size %u, dropping connection", ph.size);
                break;
            }
            payload.resize(ph.size);
            if (!net::recv_all(s, payload.data(), payload.size())) break;
            const int64_t arrival_us = now_us();
            stats.add_bytes(sizeof(ph) + payload.size());

            if (ph.flags & kPacketPong) {
                if (payload.size() == sizeof(Pong)) {
                    Pong pong;
                    std::memcpy(&pong, payload.data(), sizeof(pong));
                    clock.on_pong(pong, arrival_us);
                }
                continue;
            }

            if (ph.flags & kPacketCursor) {
                CursorUpdate cu;
                if (payload.size() < sizeof(cu)) {
                    KRG_LOG("short cursor packet, dropping connection");
                    break;
                }
                std::memcpy(&cu, payload.data(), sizeof(cu));
                size_t pixels = size_t{cu.width} * cu.height;
                size_t expected = sizeof(cu) + (cu.has_shape ? pixels * 5 : 0);
                if (payload.size() != expected ||
                    (cu.has_shape && (pixels == 0 || cu.width > 1024 || cu.height > 1024))) {
                    KRG_LOG("bogus cursor packet, dropping connection");
                    break;
                }
                if (cu.has_shape) {
                    presenter->set_cursor_shape(cu.width, cu.height, payload.data() + sizeof(cu),
                                                payload.data() + sizeof(cu) + pixels * 4);
                }
                presenter->set_cursor_pos(cu.x, cu.y, cu.visible != 0);
                frame_ready.push(FrameTiming{}); // repaint even with no video frame
                continue;
            }

            bool ok = decoder->decode(payload.data(), payload.size(),
                                      [&](ID3D11Texture2D* tex, UINT sub) {
                                          presenter->copy_video_frame(tex, sub);
                                          // One decoded frame per packet, so the
                                          // header's timings belong to this frame:
                                          // B-frames are off and both ends run in
                                          // low-latency mode.
                                          frame_ready.push(FrameTiming{ph.capture_us,
                                                                       ph.encode_us, arrival_us,
                                                                       now_us()});
                                      });
            if (!ok) {
                // A long GOP means no keyframe is coming unless we ask, so a
                // decode failure is recoverable rather than fatal — drop what
                // the decoder is holding and request an IDR. Rate-limited: a
                // burst of failures is one loss of sync, not many.
                if (arrival_us - last_resync_us > 250'000) {
                    last_resync_us = arrival_us;
                    KRG_LOG("decode failed, asking the sender for a keyframe");
                    decoder->flush();
                    control.request_keyframe();
                }
            }
        }
        net_dead = true;
        frame_ready.stop();
        CoUninitialize();
    });

    const char* codec_name = hello.codec == kCodecHevc ? "hevc" : "h264";
    uint32_t presented = 0;
    int64_t next_ping_us = 0, next_overlay_us = 0, overlay_t0_us = now_us();
    // Probe quickly at first so the offset estimate is usable within a second,
    // then back off: what it is really tracking is drift, which is slow.
    int pings_sent = 0;
    while (presenter->pump()) {
        if (net_dead) {
            KRG_LOG("connection closed");
            break;
        }

        const int64_t now = now_us();
        if (opt.stats && now >= next_ping_us) {
            control.ping(now);
            next_ping_us = now + (++pings_sent < 8 ? 150'000 : 2'000'000);
        }

        auto timing = frame_ready.pop_for(std::chrono::milliseconds(2));
        if (timing) {
            presenter->present();
            if (timing->capture_us) {
                stats.add_frame(*timing, presenter->last_present_us(), clock);
            }
            if (++presented == 1) KRG_LOG("first frame presented");
        }

        if (opt.stats && now >= next_overlay_us) {
            if (next_overlay_us) {
                presenter->set_stats_text(
                    stats.take_text(codec_name, hello.width, hello.height,
                                    presenter->scanout_delay_ms(), clock, now - overlay_t0_us)
                        .c_str());
            }
            overlay_t0_us = now;
            next_overlay_us = now + 500'000;
        }
    }

    // Shut down rather than close: the receive thread may be part-way through a
    // keyframe request on this socket, and a closed handle can be reissued to
    // another socket entirely before that send lands.
    shutdown(s, SD_BOTH);
    rx.join();
    closesocket(s);
    return 0;
}

int run(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stats") == 0) {
            opt.stats = true;
        } else if (std::strcmp(argv[i], "--smooth") == 0) {
            opt.waitable = true;
        } else if (argv[i][0] == '-') {
            KRG_LOG("usage: kilrogg-recv <host> [port] [--stats] [--smooth]");
            return 2;
        } else if (opt.host.empty()) {
            opt.host = argv[i];
        } else {
            opt.port = static_cast<uint16_t>(std::atoi(argv[i]));
        }
    }
    if (opt.host.empty()) {
        KRG_LOG("usage: kilrogg-recv <host> [port] [--stats] [--smooth]");
        return 2;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!net::init()) {
        KRG_LOG("WSAStartup failed");
        return 1;
    }

    SOCKET s = net::connect_to(opt.host, opt.port);
    if (s == INVALID_SOCKET) {
        KRG_LOG("could not connect to %s:%u", opt.host.c_str(), opt.port);
        return 1;
    }
    net::set_low_latency(s);
    // A static remote screen legitimately sends nothing for minutes, so a
    // receive timeout would be wrong here; keepalive probes tell the two
    // apart and surface a sender that vanished without closing.
    net::enable_keepalive(s, 5000, 1000);

    // The receiver speaks first: which codecs it can decode decides what the
    // sender is allowed to encode.
    ClientHello client_hello{kMagicClient, MfVideoDecoder::decodable_codecs(), 0};
    if (client_hello.codecs == 0) {
        KRG_LOG("no video decoder on this machine; only --codec lz4 senders will work");
    }
    if (!net::send_all(s, &client_hello, sizeof(client_hello))) {
        KRG_LOG("could not send hello to %s:%u", opt.host.c_str(), opt.port);
        return 1;
    }

    Hello hello;
    if (!net::recv_all(s, &hello, sizeof(hello)) ||
        (hello.magic != kMagic && hello.magic != kMagicVideo)) {
        KRG_LOG("bad handshake from %s:%u", opt.host.c_str(), opt.port);
        return 1;
    }
    KRG_LOG("connected to %s:%u, remote desktop is %ux%u (%s)", opt.host.c_str(), opt.port,
            hello.width, hello.height,
            hello.magic != kMagicVideo ? "lz4" : (hello.codec == kCodecHevc ? "hevc" : "h264"));

    return hello.magic == kMagicVideo ? run_video(s, hello, opt) : run_lz4(s, hello);
}

} // namespace
} // namespace krg

int main(int argc, char** argv) {
    return krg::run(argc, argv);
}
