#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "common/net.h"

namespace krg {

// Drains queued packets to the socket on a thread of its own, so a slow or
// stalled link never blocks the encoder's event thread. Without this, a full
// socket buffer back-pressures ProcessOutput, and every frame the encoder is
// holding ages instead of being dropped — latency grows without bound while
// the rest of the pipeline is carefully latest-wins.
//
// The queue is bounded. On overflow, *every* queued video packet is discarded
// rather than just the newest: the receiver's decoder loses sync at the first
// gap, so the remaining backlog would only paint garbage with bandwidth that
// is already scarce. The caller is told to force a keyframe instead
// (take_resync_request), which resyncs the receiver in one packet.
//
// Cursor packets are never dropped. They are tiny, and the sender only emits
// one when the cursor state version changes — a discarded shape update would
// leave the receiver drawing the wrong cursor until the shape happens to
// change again.
class PacketSender {
public:
    // Takes ownership of `socket`: it is shut down and closed on destruction.
    // `max_queued_bytes` is the backlog budget; see kilrogg-send's caller for
    // how it maps to a wall-clock tolerance at the configured bitrate.
    PacketSender(SOCKET socket, size_t max_queued_bytes);
    ~PacketSender();

    PacketSender(const PacketSender&) = delete;
    PacketSender& operator=(const PacketSender&) = delete;

    // All three copy their payload and return immediately.
    void send_video(const uint8_t* data, size_t size, bool keyframe, int64_t capture_us,
                    uint32_t encode_us);
    void send_cursor(std::span<const uint8_t> head, std::span<const uint8_t> bgra,
                     std::span<const uint8_t> invert);
    // Answers the receiver's clock probe. Like cursor packets, never dropped:
    // a pong lost to a backlog flush would just make the receiver ask again.
    void send_pong(int64_t client_us, int64_t server_us);

    // Set once the socket has failed; the caller should drop the connection.
    bool dead() const { return dead_.load(std::memory_order_relaxed); }

    // True once per backlog flush, and cleared by the read: the caller must
    // ask the encoder for a keyframe so the receiver can resync.
    bool take_resync_request();

    struct Stats {
        uint64_t packets = 0, bytes = 0; // what actually reached the socket
        uint64_t dropped_packets = 0, dropped_bytes = 0;
        uint64_t backlog_flushes = 0;
        size_t peak_queued_bytes = 0; // high-water mark since the last read
        size_t queued_bytes = 0;      // current depth, not a delta
    };
    // Returns the counters accumulated since the previous call and resets them.
    Stats take_stats();

private:
    struct Packet {
        std::vector<uint8_t> bytes; // VideoPacketHeader followed by payload
        bool video = false;         // droppable when the backlog overflows
    };

    void run();
    void push(std::vector<uint8_t> bytes, bool video, bool keyframe);
    // Header and payload land in one buffer so each packet is a single send();
    // with TCP_NODELAY on, sending the header separately would put a runt
    // segment on the wire ahead of every frame.
    static std::vector<uint8_t> build(uint32_t flags, int64_t capture_us, uint32_t encode_us,
                                      std::span<const uint8_t> a, std::span<const uint8_t> b,
                                      std::span<const uint8_t> c);

    SOCKET socket_;
    const size_t max_queued_bytes_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Packet> queue_;
    size_t queued_bytes_ = 0;
    bool stop_ = false;
    bool resync_requested_ = false;
    Stats stats_;

    std::atomic<bool> dead_{false};
    std::thread thread_;
};

} // namespace krg
