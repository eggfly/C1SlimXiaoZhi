#include "net/udp_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "platform/log.h"

#define TAG "Udp"

namespace c1xz {

UdpSocket::UdpSocket() = default;

UdpSocket::~UdpSocket() { Close(); }

void UdpSocket::OnMessage(MessageHandler handler) { on_message_ = std::move(handler); }

bool UdpSocket::Connect(const std::string& host, int port) {
    Close();
    stopping_.store(false);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    std::string port_text = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        error_ = std::string("resolve ") + host + " failed: " + gai_strerror(rc);
        C1XZ_LOGE(TAG, "%s", error_.c_str());
        return false;
    }

    int fd = -1;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        // Connect so we can use send/recv and the kernel filters the source.
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        error_ = "failed to open a UDP socket to " + host;
        C1XZ_LOGE(TAG, "%s", error_.c_str());
        return false;
    }

    fd_.store(fd);
    receive_thread_ = std::thread([this] { ReceiveLoop(); });
    C1XZ_LOGI(TAG, "udp channel to %s:%d open", host.c_str(), port);
    return true;
}

bool UdpSocket::Send(const void* data, size_t length) {
    int fd = fd_.load();
    if (fd < 0) {
        return false;
    }
    ssize_t n = ::send(fd, data, length, 0);
    return n == static_cast<ssize_t>(length);
}

void UdpSocket::ReceiveLoop() {
    // One datagram carries one Opus packet plus a 16-byte header, so this is
    // far larger than anything the server sends.
    char buffer[2048];
    while (!stopping_.load()) {
        int fd = fd_.load();
        if (fd < 0) {
            break;
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int rc = poll(&pfd, 1, 300);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (rc == 0) {
            continue;
        }
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            break;
        }
        if (n > 0 && on_message_) {
            on_message_(buffer, static_cast<size_t>(n));
        }
    }
}

void UdpSocket::Close() {
    stopping_.store(true);
    int fd = fd_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (receive_thread_.joinable()) {
        if (receive_thread_.get_id() == std::this_thread::get_id()) {
            receive_thread_.detach();
        } else {
            receive_thread_.join();
        }
    }
}

}  // namespace c1xz
