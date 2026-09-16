#ifndef C1XZ_NET_UDP_SOCKET_H
#define C1XZ_NET_UDP_SOCKET_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>

namespace c1xz {

// Connected UDP socket with a receive thread. Carries the encrypted audio
// stream of the MQTT+UDP transport.
class UdpSocket {
public:
    using MessageHandler = std::function<void(const char* data, size_t len)>;

    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void OnMessage(MessageHandler handler);

    bool Connect(const std::string& host, int port);
    bool Send(const void* data, size_t length);
    void Close();
    bool IsConnected() const { return fd_ >= 0; }

    const std::string& error() const { return error_; }

private:
    std::atomic<int> fd_{-1};
    std::atomic<bool> stopping_{false};
    std::thread receive_thread_;
    MessageHandler on_message_;
    std::string error_;

    void ReceiveLoop();
};

}  // namespace c1xz

#endif  // C1XZ_NET_UDP_SOCKET_H
