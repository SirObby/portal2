// ControlSocket: minimal blocking TCP server for the newline-delimited
// JSON control protocol described in docs/protocol.md.

#pragma once

#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

namespace RLTransport {

class ControlSocket {
public:
    ControlSocket() = default;
    ~ControlSocket() { Close(); }

    bool Listen(uint16_t port) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        m_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen_fd == kInvalidSocket) return false;

        int opt = 1;
#ifdef _WIN32
        setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR,
                    reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
        setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);

        if (bind(m_listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            Close();
            return false;
        }
        if (listen(m_listen_fd, 1) != 0) {
            Close();
            return false;
        }
        return true;
    }

    bool AcceptClient() {
        m_client_fd = accept(m_listen_fd, nullptr, nullptr);
        if (m_client_fd == kInvalidSocket) return false;

        int one = 1;
#ifdef _WIN32
        setsockopt(m_client_fd, IPPROTO_TCP, TCP_NODELAY,
                    reinterpret_cast<const char *>(&one), sizeof(one));
#else
        setsockopt(m_client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
        return true;
    }

    bool Send(const std::string &data) {
        if (m_client_fd == kInvalidSocket) return false;
        size_t total = 0;
        while (total < data.size()) {
#ifdef _WIN32
            int n = send(m_client_fd, data.data() + total,
                          static_cast<int>(data.size() - total), 0);
#else
            ssize_t n = send(m_client_fd, data.data() + total,
                              data.size() - total, 0);
#endif
            if (n <= 0) return false;
            total += static_cast<size_t>(n);
        }
        return true;
    }

    bool SendLine(const std::string &json_line) {
        return Send(json_line + "\n");
    }

    bool RecvLine(std::string *out_line) {
        while (true) {
            size_t pos = m_recv_buf.find('\n');
            if (pos != std::string::npos) {
                *out_line = m_recv_buf.substr(0, pos);
                m_recv_buf.erase(0, pos + 1);
                return true;
            }

            char chunk[4096];
#ifdef _WIN32
            int n = recv(m_client_fd, chunk, sizeof(chunk), 0);
#else
            ssize_t n = recv(m_client_fd, chunk, sizeof(chunk), 0);
#endif
            if (n <= 0) return false;
            m_recv_buf.append(chunk, static_cast<size_t>(n));
        }
    }

    void Close() {
        if (m_client_fd != kInvalidSocket) {
#ifdef _WIN32
            closesocket(m_client_fd);
#else
            close(m_client_fd);
#endif
            m_client_fd = kInvalidSocket;
        }
        if (m_listen_fd != kInvalidSocket) {
#ifdef _WIN32
            closesocket(m_listen_fd);
            WSACleanup();
#else
            close(m_listen_fd);
#endif
            m_listen_fd = kInvalidSocket;
        }
    }

private:
    socket_t m_listen_fd = kInvalidSocket;
    socket_t m_client_fd = kInvalidSocket;
    std::string m_recv_buf;
};

}  // namespace RLTransport
