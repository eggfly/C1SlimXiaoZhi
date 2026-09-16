#ifndef C1XZ_PROTOCOLS_MQTT_PROTOCOL_H
#define C1XZ_PROTOCOLS_MQTT_PROTOCOL_H

#include <mbedtls/aes.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include "net/mqtt_client.h"
#include "net/udp_socket.h"
#include "protocols/protocol.h"

#define MQTT_RECONNECT_INTERVAL_MS 60000

// Ported from xiaozhi-esp32 main/protocols/mqtt_protocol.*.
//
// Control messages go over MQTT, audio over UDP encrypted with AES-128-CTR.
// The 16-byte packet header doubles as the CTR nonce:
//   |type 1|flags 1|payload_len 2|ssrc 4|timestamp 4|sequence 4|
class MqttProtocol : public Protocol {
public:
    MqttProtocol();
    ~MqttProtocol() override;

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    static constexpr size_t kAudioHeaderSize = 16;

    std::string publish_topic_;
    std::string subscribe_topic_;

    mutable std::mutex channel_mutex_;
    std::mutex crypto_mutex_;
    std::unique_ptr<c1xz::MqttClient> mqtt_;
    std::shared_ptr<c1xz::UdpSocket> udp_;

    mbedtls_aes_context aes_ctx_;
    bool aes_ready_ = false;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_ = 0;
    uint32_t local_sequence_ = 0;
    uint32_t remote_sequence_ = 0;

    std::mutex hello_mutex_;
    std::condition_variable hello_cv_;
    bool server_hello_received_ = false;

    bool StartMqttClient(bool report_error);
    void ParseServerHello(const cJSON* root);
    void HandleUdpDatagram(const char* data, size_t len);
    static bool DecodeHexString(const std::string& hex, std::string* decoded);
    bool CryptAesCtr(const uint8_t* input, size_t input_size, const uint8_t* nonce,
                     uint8_t* output);

    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif  // C1XZ_PROTOCOLS_MQTT_PROTOCOL_H
