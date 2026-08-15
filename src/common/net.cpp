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

void set_recv_timeout(SOCKET s, uint32_t seconds) {
    DWORD ms = seconds * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
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

namespace {

// Connects with a deadline. The socket goes non-blocking for the attempt and
// back to blocking on success, so callers get an ordinary blocking socket
// either way.
bool connect_within(SOCKET s, const sockaddr* addr, int addr_len, uint32_t timeout_secs) {
    if (timeout_secs == 0) return connect(s, addr, addr_len) == 0;

    u_long nonblocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonblocking) != 0) return connect(s, addr, addr_len) == 0;

    bool connected = connect(s, addr, addr_len) == 0;
    if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_SET(s, &writable);
        FD_ZERO(&failed);
        FD_SET(s, &failed);
        timeval tv{static_cast<long>(timeout_secs), 0};
        // A refused connection arrives on the exception set, not the write set,
        // and a socket that is merely writable may still have failed — so the
        // verdict comes from SO_ERROR rather than from which set fired.
        if (select(0, nullptr, &writable, &failed, &tv) > 0) {
            int err = 0;
            int len = sizeof(err);
            connected = getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) ==
                            0 &&
                        err == 0;
        }
    }

    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);
    return connected;
}

} // namespace

SOCKET connect_to(const std::string& host, uint16_t port, uint32_t timeout_secs, bool quiet) {
    char portstr[8];
    std::snprintf(portstr, sizeof(portstr), "%u", port);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || !res) {
        if (!quiet) KRG_LOG("could not resolve host '%s'", host.c_str());
        return INVALID_SOCKET;
    }

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect_within(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen), timeout_secs)) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

std::string peer_address(SOCKET s) {
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return {};
    char buf[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf))) return {};
    return buf;
}

} // namespace krg::net
