#include "sender/control_receiver.h"

#include <cstring>

#include "common/clock.h"
#include "common/log.h"
#include "common/protocol.h"

namespace krg {

ControlReceiver::ControlReceiver(SOCKET socket, PacketSender& sender)
    : socket_(socket), sender_(sender) {
    thread_ = std::thread([this] { run(); });
}

ControlReceiver::~ControlReceiver() {
    // Half-close: recv returns 0 and the thread falls out. The socket itself
    // stays open for the PacketSender to shut down and close.
    shutdown(socket_, SD_RECEIVE);
    if (thread_.joinable()) thread_.join();
}

bool ControlReceiver::take_keyframe_request() {
    return keyframe_requested_.exchange(false, std::memory_order_relaxed);
}

void ControlReceiver::run() {
    uint8_t payload[kMaxControlPayload];
    for (;;) {
        ControlHeader header;
        if (!net::recv_all(socket_, &header, sizeof(header))) break;
        if (header.size > sizeof(payload)) {
            KRG_LOG("oversized control message (%u bytes), dropping connection", header.size);
            break;
        }
        if (header.size && !net::recv_all(socket_, payload, header.size)) break;

        switch (header.type) {
        case kControlKeyframe:
            keyframe_requested_.store(true, std::memory_order_relaxed);
            break;
        case kControlPing:
            if (header.size == sizeof(int64_t)) {
                int64_t client_us;
                std::memcpy(&client_us, payload, sizeof(client_us));
                sender_.send_pong(client_us, now_us());
            }
            break;
        default:
            break; // forward compatibility: unknown types are skipped, not fatal
        }
    }
    closed_.store(true, std::memory_order_relaxed);
}

} // namespace krg
