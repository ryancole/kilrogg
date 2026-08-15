#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string>

namespace krg::net {

bool init(); // WSAStartup; call once per process

// Disables Nagle's algorithm (TCP_NODELAY) — small frame packets must go out
// immediately or latency spikes by up to 200ms on partial segments.
void set_low_latency(SOCKET s);

// Fails a send that cannot push a single byte for `seconds`. A timeout leaves
// an unknown number of bytes delivered, so callers must treat it as fatal to
// the connection rather than retrying.
void set_send_timeout(SOCKET s, uint32_t seconds);

// Probes an idle peer so a receiver that vanished without closing (power loss,
// unplugged cable) surfaces as a socket error in seconds instead of after
// TCP's multi-minute retransmission timeout. A static screen legitimately
// produces no traffic for minutes, which is why this is keepalive rather than
// a receive timeout.
void enable_keepalive(SOCKET s, uint32_t idle_ms, uint32_t interval_ms);

bool send_all(SOCKET s, const void* data, size_t len);
bool recv_all(SOCKET s, void* data, size_t len);

SOCKET listen_on(uint16_t port);
SOCKET accept_client(SOCKET listener);
SOCKET connect_to(const std::string& host, uint16_t port);

} // namespace krg::net
