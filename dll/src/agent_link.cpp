#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "sfe/agent_link.hpp"
#include "sfe/logger.hpp"

#include <cstdlib>
#include <cstring>

namespace sfe {
namespace agent {
namespace {

SOCKET s_sock = INVALID_SOCKET;
bool   s_wsa  = false;
// Bytes received past the last complete line: a TCP read can end mid-line, and
// dropping the tail would desynchronise every message after it.
char   s_rx[65536];
int    s_rx_n = 0;

bool parseTarget(char* host, int host_cap, unsigned short* port) {
    const char* v = getenv("SFE_AGENT");
    if (!v || !*v) return false;
    const char* colon = strrchr(v, ':');
    if (!colon || colon == v) {
        sfe::log("agent: SFE_AGENT=%s is not host:port", v);
        return false;
    }
    const int hn = static_cast<int>(colon - v);
    if (hn >= host_cap) return false;
    memcpy(host, v, hn);
    host[hn] = 0;
    const long p = strtol(colon + 1, nullptr, 10);
    if (p <= 0 || p > 65535) return false;
    *port = static_cast<unsigned short>(p);
    return true;
}

} // namespace

bool requested() {
    const char* v = getenv("SFE_AGENT");
    return v && *v;
}

bool connected() { return s_sock != INVALID_SOCKET; }

bool connect(int retry_ms) {
    if (connected()) return true;
    char host[256];
    unsigned short port = 0;
    if (!parseTarget(host, sizeof(host), &port)) return false;
    if (!s_wsa) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
            sfe::log("agent: WSAStartup failed");
            return false;
        }
        s_wsa = true;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        sfe::log("agent: %s is not a dotted IPv4 address", host);
        return false;
    }
    const DWORD t0 = GetTickCount();
    for (int attempt = 1;; ++attempt) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            // Lockstep: every message is one small line waiting on a reply, so
            // Nagle's delay would be paid on every single decision.
            BOOL one = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                       sizeof(one));
            s_sock = s;
            s_rx_n = 0;
            sfe::log("agent: connected to %s:%u (attempt %d)", host, port, attempt);
            return true;
        }
        closesocket(s);
        if (GetTickCount() - t0 > static_cast<DWORD>(retry_ms)) {
            sfe::log("agent: could not connect to %s:%u within %d ms", host, port, retry_ms);
            return false;
        }
        Sleep(200);
    }
}

void close() {
    if (s_sock != INVALID_SOCKET) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        sfe::log("agent: link closed");
    }
    s_rx_n = 0;
}

bool sendAll(const char* data, int n) {
    if (!connected()) return false;
    while (n > 0) {
        const int k = send(s_sock, data, n, 0);
        if (k <= 0) {
            sfe::log("agent: send failed (WSA %d)", WSAGetLastError());
            close();
            return false;
        }
        data += k;
        n -= k;
    }
    return true;
}

bool recvLine(char* buf, int cap, int timeout_ms) {
    if (!connected()) return false;
    const DWORD t0 = GetTickCount();
    for (;;) {
        if (char* nl = static_cast<char*>(memchr(s_rx, '\n', s_rx_n))) {
            const int len = static_cast<int>(nl - s_rx);
            if (len >= cap) {
                sfe::log("agent: line of %d bytes does not fit in %d", len, cap);
                close();
                return false;
            }
            memcpy(buf, s_rx, len);
            buf[len] = 0;
            s_rx_n -= len + 1;
            memmove(s_rx, nl + 1, s_rx_n);
            return true;
        }
        if (s_rx_n == static_cast<int>(sizeof(s_rx))) {
            sfe::log("agent: %d bytes with no newline", s_rx_n);
            close();
            return false;
        }
        const DWORD spent = GetTickCount() - t0;
        if (spent >= static_cast<DWORD>(timeout_ms)) return false;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s_sock, &rd);
        timeval tv;
        const DWORD left = static_cast<DWORD>(timeout_ms) - spent;
        tv.tv_sec  = static_cast<long>(left / 1000);
        tv.tv_usec = static_cast<long>((left % 1000) * 1000);
        const int r = select(0, &rd, nullptr, nullptr, &tv);
        if (r == 0) return false;
        if (r < 0) {
            sfe::log("agent: select failed (WSA %d)", WSAGetLastError());
            close();
            return false;
        }
        const int k = recv(s_sock, s_rx + s_rx_n, static_cast<int>(sizeof(s_rx)) - s_rx_n, 0);
        if (k <= 0) {
            sfe::log("agent: the agent closed the link");
            close();
            return false;
        }
        s_rx_n += k;
    }
}

} // namespace agent
} // namespace sfe
