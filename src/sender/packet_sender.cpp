#include "sender/packet_sender.h"

#include <algorithm>

#include "common/log.h"
#include "common/protocol.h"

namespace krg {

PacketSender::PacketSender(SOCKET socket, size_t max_queued_bytes)
    : socket_(socket), max_queued_bytes_(max_queued_bytes) {
    thread_ = std::thread([this] { run(); });
}

PacketSender::~PacketSender() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        queue_.clear();
        queued_bytes_ = 0;
    }
    cv_.notify_all();
    // Unblocks a send parked on a full socket buffer. Closing the socket while
    // the thread is inside send() would be a use-after-free, so shut down,
    // join, then close.
    shutdown(socket_, SD_BOTH);
    if (thread_.joinable()) thread_.join();
    closesocket(socket_);
}

std::vector<uint8_t> PacketSender::build(uint32_t flags, int64_t capture_us, uint32_t encode_us,
                                         std::span<const uint8_t> a, std::span<const uint8_t> b,
                                         std::span<const uint8_t> c) {
    VideoPacketHeader ph{static_cast<uint32_t>(a.size() + b.size() + c.size()), flags, capture_us,
                         encode_us};
    const auto* head = reinterpret_cast<const uint8_t*>(&ph);

    std::vector<uint8_t> out;
    out.reserve(sizeof(ph) + ph.size);
    out.insert(out.end(), head, head + sizeof(ph));
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    out.insert(out.end(), c.begin(), c.end());
    return out;
}

void PacketSender::send_video(const uint8_t* data, size_t size, bool keyframe, int64_t capture_us,
                              uint32_t encode_us) {
    if (dead()) return;
    push(build(keyframe ? kPacketKeyframe : 0, capture_us, encode_us, {data, size}, {}, {}), true,
         keyframe);
}

void PacketSender::send_cursor(std::span<const uint8_t> head, std::span<const uint8_t> bgra,
                               std::span<const uint8_t> invert) {
    if (dead()) return;
    push(build(kPacketCursor, 0, 0, head, bgra, invert), false, false);
}

void PacketSender::send_pong(int64_t client_us, int64_t server_us) {
    if (dead()) return;
    Pong pong{client_us, server_us};
    std::span body{reinterpret_cast<const uint8_t*>(&pong), sizeof(pong)};
    push(build(kPacketPong, 0, 0, body, {}, {}), false, false);
}

void PacketSender::push(std::vector<uint8_t> bytes, bool video, bool keyframe) {
    {
        std::lock_guard lock(mutex_);
        // Nothing is draining the queue once either is set.
        if (stop_ || dead()) return;

        // Sampled before the overflow check below, which is precisely the
        // depth a flush is about to erase — measuring afterwards would hide
        // the backlog in the one case the high-water mark exists to show.
        stats_.peak_queued_bytes = std::max(stats_.peak_queued_bytes, queued_bytes_ + bytes.size());

        if (video && queued_bytes_ + bytes.size() > max_queued_bytes_) {
            for (auto it = queue_.begin(); it != queue_.end();) {
                if (!it->video) {
                    ++it;
                    continue;
                }
                ++stats_.dropped_packets;
                stats_.dropped_bytes += it->bytes.size();
                queued_bytes_ -= it->bytes.size();
                it = queue_.erase(it);
            }
            ++stats_.backlog_flushes;

            // A keyframe is self-contained, so it is worth queueing even now —
            // it resyncs the receiver on its own. Anything else references
            // frames the receiver will never see, so drop it and let the run
            // loop force a fresh keyframe instead.
            if (!keyframe) {
                ++stats_.dropped_packets;
                stats_.dropped_bytes += bytes.size();
                resync_requested_ = true;
                return;
            }
        }

        queued_bytes_ += bytes.size();
        queue_.push_back({std::move(bytes), video});
    }
    cv_.notify_one();
}

void PacketSender::run() {
    for (;;) {
        Packet packet;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            packet = std::move(queue_.front());
            queue_.pop_front();
            queued_bytes_ -= packet.bytes.size();
        }

        // Deliberately outside the lock: this is the call that blocks when the
        // link backs up, and producers must stay free to queue and drop.
        if (!net::send_all(socket_, packet.bytes.data(), packet.bytes.size())) {
            int err = WSAGetLastError();
            KRG_LOG("send failed (error %d)%s", err,
                    err == WSAETIMEDOUT ? " — link stalled, dropping connection" : "");
            dead_ = true;
            std::lock_guard lock(mutex_);
            queue_.clear();
            queued_bytes_ = 0;
            return;
        }

        std::lock_guard lock(mutex_);
        ++stats_.packets;
        stats_.bytes += packet.bytes.size();
    }
}

bool PacketSender::take_resync_request() {
    std::lock_guard lock(mutex_);
    bool requested = resync_requested_;
    resync_requested_ = false;
    return requested;
}

PacketSender::Stats PacketSender::take_stats() {
    std::lock_guard lock(mutex_);
    Stats out = stats_;
    out.queued_bytes = queued_bytes_;
    stats_ = Stats{};
    return out;
}

} // namespace krg
