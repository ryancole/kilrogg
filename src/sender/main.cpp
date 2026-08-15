#include <windows.h>

#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
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
#include "sender/mf_encoder.h"

using Microsoft::WRL::ComPtr;

namespace krg {
namespace {

struct Options {
    bool dummy = false;
    bool lz4 = false;
    uint16_t port = kDefaultPort;
    uint32_t bitrate_mbps = 40;
};

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

// ---------------------------------------------------------------------------
// KRG2 path: hardware H.264. Frames stay on the GPU from capture to encoder.
// ---------------------------------------------------------------------------

// Cursor packets go out on the run loop's thread while video packets go out
// on the encoder's event thread — `send_mutex` keeps the two packet streams
// from interleaving mid-packet.
bool send_cursor(SOCKET s, std::mutex& send_mutex, const CursorPos& pos,
                 const CursorShape* shape) {
    CursorUpdate cu{};
    cu.x = pos.x;
    cu.y = pos.y;
    cu.visible = pos.visible ? 1 : 0;
    size_t extra = 0;
    if (shape) {
        cu.has_shape = 1;
        cu.width = shape->width;
        cu.height = shape->height;
        extra = shape->bgra.size() + shape->invert.size();
    }
    VideoPacketHeader ph{static_cast<uint32_t>(sizeof(cu) + extra), kPacketCursor};
    std::lock_guard lock(send_mutex);
    if (!net::send_all(s, &ph, sizeof(ph)) || !net::send_all(s, &cu, sizeof(cu))) return false;
    if (shape) {
        if (!net::send_all(s, shape->bgra.data(), shape->bgra.size())) return false;
        if (!net::send_all(s, shape->invert.data(), shape->invert.size())) return false;
    }
    return true;
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

    auto encoder =
        MfH264Encoder::create(device, w, h, 60, opt.bitrate_mbps * 1'000'000);
    if (!encoder) return 1;

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
        net::set_low_latency(client);
        KRG_LOG("client connected");

        Hello hello{kMagicVideo, w, h};
        if (!net::send_all(client, &hello, sizeof(hello))) {
            closesocket(client);
            continue;
        }

        std::atomic<bool> dead{false};
        std::mutex send_mutex;
        auto stat_t0 = std::chrono::steady_clock::now();
        uint32_t stat_frames = 0;
        int64_t stat_bytes = 0;

        encoder->set_sink([&](const uint8_t* data, size_t size, bool keyframe) {
            VideoPacketHeader ph{static_cast<uint32_t>(size), keyframe ? kPacketKeyframe : 0};
            std::lock_guard lock(send_mutex);
            if (!net::send_all(client, &ph, sizeof(ph)) || !net::send_all(client, data, size)) {
                dead = true;
                return;
            }
            ++stat_frames;
            stat_bytes += sizeof(ph) + size;
            auto now = std::chrono::steady_clock::now();
            if (now - stat_t0 >= std::chrono::seconds(5)) {
                double secs = std::chrono::duration<double>(now - stat_t0).count();
                KRG_LOG("%.1f fps, %.2f Mbit/s", stat_frames / secs,
                        stat_bytes * 8.0 / 1e6 / secs);
                stat_frames = 0;
                stat_bytes = 0;
                stat_t0 = now;
            }
        });
        encoder->request_keyframe();

        // Async encoders hold a pipeline of frames and only emit frame N when
        // frame N+1 arrives, so sparse content (a desktop with occasional
        // changes) would sit in the encoder for seconds. On a quiet tick,
        // re-submit the last frame to flush changes through — but only for a
        // bounded burst, so a truly static screen stops producing traffic.
        ComPtr<ID3D11Texture2D> last_tex;
        int flush_budget = 0;
        // Version 0 = "never sent to this client": the first poll always
        // delivers the current shape and position to a fresh connection.
        uint64_t cursor_pos_ver = 0, cursor_shape_ver = 0;
        CursorPos cursor_pos;
        while (!dead) {
            if (capture_source) {
                CursorShape shape;
                bool shape_changed = capture_source->poll_cursor_shape(cursor_shape_ver, shape);
                bool pos_changed = capture_source->poll_cursor_pos(cursor_pos_ver, cursor_pos);
                if ((shape_changed || pos_changed) &&
                    !send_cursor(client, send_mutex, cursor_pos,
                                 shape_changed ? &shape : nullptr)) {
                    break;
                }
            }
            auto tex = mailbox.pop_for(std::chrono::milliseconds(16));
            if (tex) {
                last_tex = std::move(*tex);
                flush_budget = 30;
            } else {
                if (flush_budget <= 0 || !last_tex) continue;
                --flush_budget;
            }
            if (!encoder->encode(last_tex.Get())) break;
        }
        encoder->set_sink(nullptr);
        closesocket(client);
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
            if (std::strcmp(argv[i], "lz4") == 0) opt.lz4 = true;
            else if (std::strcmp(argv[i], "h264") != 0) {
                KRG_LOG("unknown codec '%s' (h264|lz4)", argv[i]);
                return 2;
            }
        } else if (std::strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            opt.bitrate_mbps = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else {
            KRG_LOG("usage: kilrogg-send [--dummy] [--port N] [--codec h264|lz4] [--bitrate Mbps]");
            return 2;
        }
    }

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

    KRG_LOG("listening on port %u (h264, %u Mbit/s target)", opt.port, opt.bitrate_mbps);
    return run_h264(opt, listener);
}

} // namespace
} // namespace krg

int main(int argc, char** argv) {
    return krg::run(argc, argv);
}
