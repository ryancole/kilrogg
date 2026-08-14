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

bool send_all(SOCKET s, const void* data, size_t len);
bool recv_all(SOCKET s, void* data, size_t len);

SOCKET listen_on(uint16_t port);
SOCKET accept_client(SOCKET listener);
SOCKET connect_to(const std::string& host, uint16_t port);

} // namespace krg::net
