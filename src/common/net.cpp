#include "common/net.h"

#include <mstcpip.h>

#include <algorithm>
#include <cstdio>

#include "common/log.h"

namespace krg::net {

bool init() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void set_low_latency(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

void set_send_timeout(SOCKET s, uint32_t seconds) {
    DWORD ms = seconds * 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

void enable_keepalive(SOCKET s, uint32_t idle_ms, uint32_t interval_ms) {
    tcp_keepalive ka{};
    ka.onoff = 1;
    ka.keepalivetime = idle_ms;
    ka.keepaliveinterval = interval_ms;
    DWORD returned = 0;
    if (WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &returned, nullptr,
                 nullptr) != 0) {
        KRG_LOG("keepalive setup failed (error %d)", WSAGetLastError());
    }
}

bool send_all(SOCKET s, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        int chunk = static_cast<int>(std::min(len, size_t{1} << 20));
        int n = send(s, p, chunk, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(SOCKET s, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len > 0) {
        int chunk = static_cast<int>(std::min(len, size_t{1} << 20));
        int n = recv(s, p, chunk, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

SOCKET listen_on(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 1) != 0) {
        KRG_LOG("bind/listen on port %u failed (error %d)", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

SOCKET accept_client(SOCKET listener) {
    return accept(listener, nullptr, nullptr);
}

SOCKET connect_to(const std::string& host, uint16_t port) {
    char portstr[8];
    std::snprintf(portstr, sizeof(portstr), "%u", port);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || !res) {
        KRG_LOG("could not resolve host '%s'", host.c_str());
        return INVALID_SOCKET;
    }

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

} // namespace krg::net
