#include "net/mqtt_client.h"

#include <cstring>

#include "net/tls_socket.h"
#include "platform/event_loop.h"
#include "platform/log.h"

#define TAG "Mqtt"

namespace c1xz {
namespace {

constexpr uint8_t kConnect = 1;
constexpr uint8_t kConnAck = 2;
constexpr uint8_t kPublish = 3;
constexpr uint8_t kSubscribe = 8;
constexpr uint8_t kSubAck = 9;
constexpr uint8_t kPingReq = 12;
constexpr uint8_t kPingResp = 13;
constexpr uint8_t kDisconnect = 14;

void AppendRemainingLength(std::string* out, size_t length) {
    do {
        uint8_t byte = length % 128;
        length /= 128;
        if (length > 0) {
            byte |= 0x80;
        }
        out->push_back(static_cast<char>(byte));
    } while (length > 0);
}

void AppendString(std::string* out, const std::string& value) {
    out->push_back(static_cast<char>((value.size() >> 8) & 0xff));
    out->push_back(static_cast<char>(value.size() & 0xff));
    out->append(value);
}

const char* ConnAckReason(uint8_t code) {
    switch (code) {
        case 0: return "accepted";
        case 1: return "unacceptable protocol version";
        case 2: return "client id rejected";
        case 3: return "server unavailable";
        case 4: return "bad username or password";
        case 5: return "not authorized";
        default: return "unknown";
    }
}

}  // namespace

struct MqttClient::Impl {
    TlsSocket socket;
    std::string rx;
    int64_t last_tx_ms = 0;
};

MqttClient::MqttClient() : impl_(new Impl()) {}

MqttClient::~MqttClient() { Disconnect(); }

void MqttClient::OnMessage(MessageHandler handler) { on_message_ = std::move(handler); }
void MqttClient::OnDisconnected(VoidHandler handler) { on_disconnected_ = std::move(handler); }

void MqttClient::Fail(const std::string& message) {
    error_ = message;
    C1XZ_LOGE(TAG, "%s", message.c_str());
}

bool MqttClient::SendPacket(const std::string& packet) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!impl_->socket.connected()) {
        return false;
    }
    if (impl_->socket.Write(packet.data(), packet.size()) < 0) {
        return false;
    }
    impl_->last_tx_ms = MonotonicMs();
    return true;
}

bool MqttClient::Connect(const Options& options) {
    Disconnect();
    impl_.reset(new Impl());
    stopping_.store(false);
    error_.clear();
    keepalive_seconds_ = options.keepalive_seconds;

    TlsSocket::Options socket_options;
    socket_options.host = options.host;
    socket_options.port = options.port;
    socket_options.use_tls = options.use_tls;
    socket_options.verify_peer = options.verify_peer;
    socket_options.connect_timeout_ms = options.io_timeout_ms;
    socket_options.io_timeout_ms = options.io_timeout_ms;
    if (!impl_->socket.Connect(socket_options)) {
        Fail(impl_->socket.last_error());
        return false;
    }

    std::string payload;
    AppendString(&payload, "MQTT");
    payload.push_back(4);  // protocol level 4 == MQTT 3.1.1

    uint8_t flags = 0x02;  // clean session
    if (!options.username.empty()) {
        flags |= 0x80;
    }
    if (!options.password.empty()) {
        flags |= 0x40;
    }
    payload.push_back(static_cast<char>(flags));
    payload.push_back(static_cast<char>((options.keepalive_seconds >> 8) & 0xff));
    payload.push_back(static_cast<char>(options.keepalive_seconds & 0xff));
    AppendString(&payload, options.client_id);
    if (!options.username.empty()) {
        AppendString(&payload, options.username);
    }
    if (!options.password.empty()) {
        AppendString(&payload, options.password);
    }

    std::string packet;
    packet.push_back(static_cast<char>(kConnect << 4));
    AppendRemainingLength(&packet, payload.size());
    packet.append(payload);

    if (impl_->socket.Write(packet.data(), packet.size()) < 0) {
        Fail("failed to send CONNECT");
        return false;
    }
    impl_->last_tx_ms = MonotonicMs();

    // Wait for CONNACK: fixed header (2) + variable header (2).
    char buffer[8];
    size_t have = 0;
    while (have < 4) {
        int n = impl_->socket.Read(buffer + have, 4 - have);
        if (n <= 0) {
            Fail("no CONNACK from the broker");
            return false;
        }
        have += static_cast<size_t>(n);
    }
    if (static_cast<uint8_t>(buffer[0]) >> 4 != kConnAck) {
        Fail("expected CONNACK, got packet type " +
             std::to_string(static_cast<uint8_t>(buffer[0]) >> 4));
        return false;
    }
    uint8_t code = static_cast<uint8_t>(buffer[3]);
    if (code != 0) {
        Fail(std::string("broker refused the connection: ") + ConnAckReason(code));
        return false;
    }

    connected_.store(true);
    receive_thread_ = std::thread([this] { ReceiveLoop(); });
    C1XZ_LOGI(TAG, "connected to %s:%d as %s", options.host.c_str(), options.port,
              options.client_id.c_str());
    return true;
}

bool MqttClient::Subscribe(const std::string& topic) {
    if (!connected_.load() || topic.empty()) {
        return false;
    }
    std::string payload;
    uint16_t id = next_packet_id_++;
    if (next_packet_id_ == 0) {
        next_packet_id_ = 1;
    }
    payload.push_back(static_cast<char>((id >> 8) & 0xff));
    payload.push_back(static_cast<char>(id & 0xff));
    AppendString(&payload, topic);
    payload.push_back(0);  // QoS 0

    std::string packet;
    // SUBSCRIBE requires the reserved bits to be 0b0010.
    packet.push_back(static_cast<char>((kSubscribe << 4) | 0x02));
    AppendRemainingLength(&packet, payload.size());
    packet.append(payload);
    return SendPacket(packet);
}

bool MqttClient::Publish(const std::string& topic, const std::string& payload) {
    if (!connected_.load()) {
        return false;
    }
    std::string body;
    AppendString(&body, topic);
    body.append(payload);

    std::string packet;
    packet.push_back(static_cast<char>(kPublish << 4));  // QoS 0, no retain
    AppendRemainingLength(&packet, body.size());
    packet.append(body);
    return SendPacket(packet);
}

void MqttClient::HandlePublish(const std::string& body) {
    if (body.size() < 2) {
        return;
    }
    size_t topic_len = (static_cast<uint8_t>(body[0]) << 8) | static_cast<uint8_t>(body[1]);
    if (body.size() < 2 + topic_len) {
        return;
    }
    std::string topic = body.substr(2, topic_len);
    std::string payload = body.substr(2 + topic_len);
    if (on_message_) {
        on_message_(topic, payload);
    }
}

void MqttClient::ReceiveLoop() {
    char chunk[2048];
    while (!stopping_.load()) {
        // Keepalive: the broker drops us after 1.5x keepalive without traffic.
        if (keepalive_seconds_ > 0) {
            int64_t idle = MonotonicMs() - impl_->last_tx_ms;
            if (idle > keepalive_seconds_ * 1000 / 2) {
                std::string ping;
                ping.push_back(static_cast<char>(kPingReq << 4));
                ping.push_back(0);
                if (!SendPacket(ping)) {
                    break;
                }
            }
        }

        // Parse whole packets already buffered.
        bool progressed = false;
        while (impl_->rx.size() >= 2) {
            size_t multiplier = 1;
            size_t length = 0;
            size_t index = 1;
            bool complete = false;
            while (index < impl_->rx.size() && index <= 4) {
                uint8_t byte = static_cast<uint8_t>(impl_->rx[index]);
                length += (byte & 0x7f) * multiplier;
                multiplier *= 128;
                ++index;
                if ((byte & 0x80) == 0) {
                    complete = true;
                    break;
                }
            }
            if (!complete) {
                break;  // remaining-length field still incomplete
            }
            if (impl_->rx.size() < index + length) {
                break;
            }
            uint8_t type = static_cast<uint8_t>(impl_->rx[0]) >> 4;
            std::string body = impl_->rx.substr(index, length);
            impl_->rx.erase(0, index + length);
            progressed = true;

            switch (type) {
                case kPublish:
                    HandlePublish(body);
                    break;
                case kPingResp:
                case kSubAck:
                    break;
                default:
                    C1XZ_LOGD(TAG, "ignoring packet type %u", type);
                    break;
            }
        }

        if (stopping_.load()) {
            break;
        }
        if (!progressed && !impl_->socket.WaitReadable(300)) {
            continue;
        }
        int n = impl_->socket.Read(chunk, sizeof(chunk));
        if (n == TlsSocket::kTimeout) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        impl_->rx.append(chunk, static_cast<size_t>(n));
    }

    connected_.store(false);
    impl_->socket.Close();
    if (on_disconnected_ && !stopping_.load()) {
        on_disconnected_();
    }
}

void MqttClient::Disconnect() {
    if (!impl_) {
        return;
    }
    bool was_connected = connected_.exchange(false);
    stopping_.store(true);
    if (was_connected) {
        std::string packet;
        packet.push_back(static_cast<char>(kDisconnect << 4));
        packet.push_back(0);
        SendPacket(packet);
    }
    impl_->socket.Close();
    if (receive_thread_.joinable()) {
        if (receive_thread_.get_id() == std::this_thread::get_id()) {
            receive_thread_.detach();
        } else {
            receive_thread_.join();
        }
    }
}

}  // namespace c1xz
