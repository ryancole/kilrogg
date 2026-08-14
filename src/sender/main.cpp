#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <lz4.h>

#include "common/log.h"
#include "common/mailbox.h"
#include "common/net.h"
#include "common/protocol.h"
#include "sender/dummy_source.h"
#include "sender/dxgi_capture.h"

namespace krg {
namespace {

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

int run(int argc, char** argv) {
    bool dummy = false;
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dummy") == 0) {
            dummy = true;
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else {
            KRG_LOG("usage: kilrogg-send [--dummy] [--port N]");
            return 2;
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!net::init()) {
        KRG_LOG("WSAStartup failed");
        return 1;
    }

    std::unique_ptr<FrameSource> source;
    if (dummy) {
        source = std::make_unique<DummySource>(1280, 720);
        KRG_LOG("using dummy frame source (1280x720)");
    } else {
        source = DxgiCapture::create();
        if (!source) return 1;
    }
    const uint32_t w = source->width(), h = source->height();

    // Latest-wins handoff: if the send thread is behind, the newer frame
    // absorbs the dropped frame's dirty rects (its pixels are already the
    // full image, so nothing on screen is lost). Accumulated rects overlap —
    // a fullscreen game dirties the whole screen every frame — so once they
    // cover more pixels than the frame itself, one keyframe is strictly
    // cheaper than resending the overlap.
    Mailbox<Frame> mailbox([w, h](Frame& incoming, Frame& pending) {
        incoming.rects.insert(incoming.rects.end(), pending.rects.begin(), pending.rects.end());
        uint64_t area = 0;
        for (const Rect& r : incoming.rects) area += uint64_t{r.w} * r.h;
        if (area > uint64_t{w} * h) incoming.rects.assign(1, Rect{0, 0, w, h});
    });

    std::thread capture([&] {
        for (;;) {
            Frame f;
            if (source->next_frame(f)) mailbox.push(std::move(f));
        }
    });
    capture.detach();

    SOCKET listener = net::listen_on(port);
    if (listener == INVALID_SOCKET) return 1;
    KRG_LOG("listening on port %u, sharing %ux%u", port, w, h);

    Frame last; // most recent complete frame, reused as the keyframe on (re)connect
    for (;;) {
        SOCKET client = net::accept_client(listener);
        if (client == INVALID_SOCKET) continue;
        net::set_low_latency(client);
        KRG_LOG("client connected");

        Hello hello{kMagic, w, h};
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

} // namespace
} // namespace krg

int main(int argc, char** argv) {
    return krg::run(argc, argv);
}
