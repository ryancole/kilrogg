#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <lz4.h>

#include "common/log.h"
#include "common/mailbox.h"
#include "common/net.h"
#include "common/protocol.h"
#include "receiver/mf_decoder.h"
#include "receiver/presenter.h"

namespace krg {
namespace {

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
    auto presenter = Presenter::create(hello.width, hello.height, false);
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

    closesocket(s); // unblocks the recv thread if it's still reading
    rx.join();
    return 0;
}

// ---------------------------------------------------------------------------
// KRG2: H.264 video.
// ---------------------------------------------------------------------------

int run_h264(SOCKET s, const Hello& hello) {
    auto presenter = Presenter::create(hello.width, hello.height, true);
    if (!presenter) return 1;

    auto decoder = MfH264Decoder::create(presenter->device(), hello.width, hello.height);
    if (!decoder) return 1;

    Mailbox<bool> frame_ready; // pure signal; the NV12 texture is the state

    std::atomic<bool> net_dead{false};
    std::thread rx([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<uint8_t> payload;
        for (;;) {
            VideoPacketHeader ph;
            if (!net::recv_all(s, &ph, sizeof(ph))) break;
            if (ph.size == 0 || ph.size > (32u << 20)) {
                KRG_LOG("bogus packet size %u, dropping connection", ph.size);
                break;
            }
            payload.resize(ph.size);
            if (!net::recv_all(s, payload.data(), payload.size())) break;

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
                frame_ready.push(true); // repaint even if no video frame arrives
                continue;
            }

            bool ok = decoder->decode(payload.data(), payload.size(),
                                      [&](ID3D11Texture2D* tex, UINT sub) {
                                          presenter->copy_video_frame(tex, sub);
                                          frame_ready.push(true);
                                      });
            if (!ok) break;
        }
        net_dead = true;
        frame_ready.stop();
        CoUninitialize();
    });

    uint32_t presented = 0;
    while (presenter->pump()) {
        if (net_dead) {
            KRG_LOG("connection closed");
            break;
        }
        if (frame_ready.pop_for(std::chrono::milliseconds(2))) {
            presenter->present();
            if (++presented == 1) KRG_LOG("first frame presented");
        }
    }

    closesocket(s);
    rx.join();
    return 0;
}

int run(int argc, char** argv) {
    if (argc < 2) {
        KRG_LOG("usage: kilrogg-recv <host> [port]");
        return 2;
    }
    std::string host = argv[1];
    uint16_t port = argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : kDefaultPort;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!net::init()) {
        KRG_LOG("WSAStartup failed");
        return 1;
    }

    SOCKET s = net::connect_to(host, port);
    if (s == INVALID_SOCKET) {
        KRG_LOG("could not connect to %s:%u", host.c_str(), port);
        return 1;
    }
    net::set_low_latency(s);
    // A static remote screen legitimately sends nothing for minutes, so a
    // receive timeout would be wrong here; keepalive probes tell the two
    // apart and surface a sender that vanished without closing.
    net::enable_keepalive(s, 5000, 1000);

    Hello hello;
    if (!net::recv_all(s, &hello, sizeof(hello)) ||
        (hello.magic != kMagic && hello.magic != kMagicVideo)) {
        KRG_LOG("bad handshake from %s:%u", host.c_str(), port);
        return 1;
    }
    KRG_LOG("connected to %s:%u, remote desktop is %ux%u (%s)", host.c_str(), port, hello.width,
            hello.height, hello.magic == kMagicVideo ? "h264" : "lz4");

    return hello.magic == kMagicVideo ? run_h264(s, hello) : run_lz4(s, hello);
}

} // namespace
} // namespace krg

int main(int argc, char** argv) {
    return krg::run(argc, argv);
}
