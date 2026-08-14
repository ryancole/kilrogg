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
#include "receiver/presenter.h"

namespace krg {
namespace {

struct UpdateBatch {
    uint32_t frame_id = 0;
    std::vector<RectUpdate> updates;
};

// Reads frames off the socket, decompresses each rect, and hands batches to
// the present loop. Returns when the connection drops or data is invalid.
void receive_loop(SOCKET s, uint32_t width, uint32_t height, Mailbox<UpdateBatch>& mailbox) {
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

    Hello hello;
    if (!net::recv_all(s, &hello, sizeof(hello)) || hello.magic != kMagic) {
        KRG_LOG("bad handshake from %s:%u", host.c_str(), port);
        return 1;
    }
    KRG_LOG("connected to %s:%u, remote desktop is %ux%u", host.c_str(), port, hello.width,
            hello.height);

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
        receive_loop(s, hello.width, hello.height, mailbox);
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
            else if (presented % 300 == 0) KRG_LOG("presented %u update batches", presented);
        }
    }

    closesocket(s); // unblocks the recv thread if it's still reading
    rx.join();
    return 0;
}

} // namespace
} // namespace krg

int main(int argc, char** argv) {
    return krg::run(argc, argv);
}
