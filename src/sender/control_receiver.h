#pragma once
#include <atomic>
#include <cstdint>
#include <thread>

#include "common/net.h"
#include "sender/packet_sender.h"

namespace krg {

// Reads the receiver's back-channel on a thread of its own. The stream is
// one-way for pixels but not for control: with a long GOP, a receiver that
// has lost decoder sync has no other way to get an IDR, and the clock probes
// it sends are what turn the sender's timestamps into an end-to-end number
// rather than two unrelated stopwatches.
//
// Clock probes are answered inline (a reply delayed by the run loop's tick
// would measure the tick, not the link). Keyframe requests are handed to the
// run loop as a flag instead, so the encoder is still only driven from the
// one thread that drives it today.
//
// Does not own the socket: construct it *after* the PacketSender that does,
// so it is destroyed — and its thread joined — while the socket is still open.
class ControlReceiver {
public:
    ControlReceiver(SOCKET socket, PacketSender& sender);
    ~ControlReceiver();

    ControlReceiver(const ControlReceiver&) = delete;
    ControlReceiver& operator=(const ControlReceiver&) = delete;

    // True once per request, cleared by the read.
    bool take_keyframe_request();

    // The receiver closed the connection or sent something malformed.
    bool closed() const { return closed_.load(std::memory_order_relaxed); }

private:
    void run();

    SOCKET socket_;
    PacketSender& sender_;
    std::atomic<bool> keyframe_requested_{false};
    std::atomic<bool> closed_{false};
    std::thread thread_;
};

} // namespace krg
