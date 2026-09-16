#ifndef C1XZ_NET_MQTT_CLIENT_H
#define C1XZ_NET_MQTT_CLIENT_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace c1xz {

// MQTT 3.1.1 client, QoS 0 only.
//
// That is all the xiaozhi control channel uses: one subscribe topic, one
// publish topic, and a keepalive. Sessions are never resumed, so there is no
// need for QoS 1/2, retained messages or persistent state.
class MqttClient {
public:
    using MessageHandler =
        std::function<void(const std::string& topic, const std::string& payload)>;
    using VoidHandler = std::function<void()>;

    MqttClient();
    ~MqttClient();

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    struct Options {
        std::string host;
        int port = 8883;
        bool use_tls = true;
        bool verify_peer = true;
        std::string client_id;
        std::string username;
        std::string password;
        int keepalive_seconds = 240;
        int io_timeout_ms = 20000;
    };

    void OnMessage(MessageHandler handler);
    void OnDisconnected(VoidHandler handler);

    bool Connect(const Options& options);
    bool Subscribe(const std::string& topic);
    bool Publish(const std::string& topic, const std::string& payload);
    void Disconnect();
    bool IsConnected() const { return connected_.load(); }

    const std::string& error() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::thread receive_thread_;
    std::mutex send_mutex_;
    int keepalive_seconds_ = 240;
    uint16_t next_packet_id_ = 1;

    MessageHandler on_message_;
    VoidHandler on_disconnected_;
    std::string error_;

    bool SendPacket(const std::string& packet);
    void ReceiveLoop();
    void HandlePublish(const std::string& body);
    void Fail(const std::string& message);
};

}  // namespace c1xz

#endif  // C1XZ_NET_MQTT_CLIENT_H
