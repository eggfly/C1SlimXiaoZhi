#ifndef C1XZ_NET_WEBSOCKET_CLIENT_H
#define C1XZ_NET_WEBSOCKET_CLIENT_H

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace c1xz {

// RFC 6455 client over TlsSocket.
//
// Connect() performs the handshake on the calling thread and then starts a
// receive thread. Callbacks fire on that receive thread, so handlers must hand
// work to the application event loop rather than block.
//
// Send() is safe from any thread.
class WebSocketClient {
public:
    using DataHandler = std::function<void(const char* data, size_t len, bool binary)>;
    using VoidHandler = std::function<void()>;
    using ErrorHandler = std::function<void(const std::string& message)>;

    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    void SetHeader(const std::string& key, const std::string& value);
    void SetTimeout(int milliseconds);
    void SetVerifyPeer(bool verify);

    // Sends a ping when the link has been quiet this long. Zero disables it.
    void SetPingInterval(int milliseconds);

    void OnData(DataHandler handler);
    void OnConnected(VoidHandler handler);
    void OnDisconnected(VoidHandler handler);
    void OnError(ErrorHandler handler);

    bool Connect(const std::string& url);
    bool Send(const void* data, size_t length, bool binary);
    bool SendText(const std::string& text);
    bool IsConnected() const;
    void Close();

    const std::string& error() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::map<std::string, std::string> headers_;
    int timeout_ms_ = 20000;
    int ping_interval_ms_ = 30000;
    bool verify_peer_ = true;
    std::string error_;

    DataHandler on_data_;
    VoidHandler on_connected_;
    VoidHandler on_disconnected_;
    ErrorHandler on_error_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::thread receive_thread_;
    std::mutex send_mutex_;

    bool Handshake(const std::string& url);
    void ReceiveLoop();
    bool SendFrame(uint8_t opcode, const void* payload, size_t length);
    void Fail(const std::string& message);
};

}  // namespace c1xz

#endif  // C1XZ_NET_WEBSOCKET_CLIENT_H
