#include "protocols/mqtt_protocol.h"

#include <arpa/inet.h>
#include <cstring>

#include "audio/audio_service.h"
#include "boards/board.h"
#include "platform/log.h"
#include "settings.h"

#define TAG "MQTT"

MqttProtocol::MqttProtocol() { mbedtls_aes_init(&aes_ctx_); }

MqttProtocol::~MqttProtocol() {
    CloseAudioChannel(false);
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        mqtt_.reset();
    }
    std::lock_guard<std::mutex> lock(crypto_mutex_);
    mbedtls_aes_free(&aes_ctx_);
    aes_ready_ = false;
}

bool MqttProtocol::Start() { return StartMqttClient(false); }

bool MqttProtocol::StartMqttClient(bool report_error) {
    Settings settings("mqtt", false);
    c1xz::MqttClient::Options options;
    std::string endpoint = settings.GetString("endpoint");
    options.client_id = settings.GetString("client_id");
    options.username = settings.GetString("username");
    options.password = settings.GetString("password");
    options.keepalive_seconds = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");
    subscribe_topic_ = settings.GetString("subscribe_topic");

    if (endpoint.empty()) {
        C1XZ_LOGE(TAG, "no MQTT endpoint configured; the OTA check must run first");
        if (report_error) {
            SetError("尚未获取服务器地址");
        }
        return false;
    }

    // The endpoint is "host" or "host:port"; TLS is the default.
    options.host = endpoint;
    options.port = 8883;
    size_t colon = endpoint.rfind(':');
    if (colon != std::string::npos) {
        options.host = endpoint.substr(0, colon);
        options.port = atoi(endpoint.c_str() + colon + 1);
    }
    options.use_tls = options.port != 1883;

    std::lock_guard<std::mutex> lock(channel_mutex_);
    mqtt_.reset(new c1xz::MqttClient());
    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        (void)topic;
        cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
        if (root == nullptr) {
            C1XZ_LOGE(TAG, "unparseable MQTT payload of %zu bytes", payload.size());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(type)) {
            if (strcmp(type->valuestring, "hello") == 0) {
                ParseServerHello(root);
            } else if (strcmp(type->valuestring, "goodbye") == 0) {
                // The server closed the session; do not answer with a goodbye.
                CloseAudioChannel(false);
            } else if (on_incoming_json_ != nullptr) {
                on_incoming_json_(root);
            }
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });
    mqtt_->OnDisconnected([this]() {
        C1XZ_LOGW(TAG, "MQTT connection lost");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    if (!mqtt_->Connect(options)) {
        C1XZ_LOGE(TAG, "MQTT connect failed: %s", mqtt_->error().c_str());
        if (report_error) {
            SetError("无法连接服务器", mqtt_->error());
        }
        mqtt_.reset();
        return false;
    }
    if (!subscribe_topic_.empty()) {
        mqtt_->Subscribe(subscribe_topic_);
    }
    if (on_connected_ != nullptr) {
        on_connected_();
    }
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (mqtt_ == nullptr || !mqtt_->IsConnected() || publish_topic_.empty()) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        C1XZ_LOGE(TAG, "failed to publish: %s", text.c_str());
        SetError("服务器连接中断");
        return false;
    }
    return true;
}

bool MqttProtocol::DecodeHexString(const std::string& hex, std::string* decoded) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    decoded->clear();
    decoded->reserve(hex.size() / 2);
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = value(hex[i]);
        int lo = value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        decoded->push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

bool MqttProtocol::CryptAesCtr(const uint8_t* input, size_t input_size, const uint8_t* nonce,
                               uint8_t* output) {
    std::lock_guard<std::mutex> lock(crypto_mutex_);
    if (!aes_ready_ || input == nullptr || nonce == nullptr || output == nullptr) {
        return false;
    }
    // CTR is symmetric, so this one call both encrypts and decrypts. Each
    // packet restarts the counter from its own header, so nc_off starts at 0
    // and the working copies must not be shared between packets.
    size_t nc_off = 0;
    unsigned char nonce_counter[16];
    unsigned char stream_block[16];
    memcpy(nonce_counter, nonce, sizeof(nonce_counter));
    memset(stream_block, 0, sizeof(stream_block));
    int rc = mbedtls_aes_crypt_ctr(&aes_ctx_, input_size, &nc_off, nonce_counter, stream_block,
                                   input, output);
    if (rc != 0) {
        C1XZ_LOGE(TAG, "AES-CTR failed: -0x%04x", -rc);
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::shared_ptr<c1xz::UdpSocket> udp;
    std::string nonce;
    uint32_t sequence;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (udp_ == nullptr) {
            return false;
        }
        if (aes_nonce_.size() != kAudioHeaderSize || packet->payload.size() > UINT16_MAX) {
            C1XZ_LOGE(TAG, "bad nonce or oversized payload (%zu bytes)", packet->payload.size());
            return false;
        }
        udp = udp_;
        nonce = aes_nonce_;
        sequence = htonl(++local_sequence_);
    }

    const uint16_t payload_len = htons(static_cast<uint16_t>(packet->payload.size()));
    const uint32_t timestamp = htonl(packet->timestamp);
    memcpy(&nonce[2], &payload_len, sizeof(payload_len));
    memcpy(&nonce[8], &timestamp, sizeof(timestamp));
    memcpy(&nonce[12], &sequence, sizeof(sequence));

    std::string datagram;
    datagram.resize(nonce.size() + packet->payload.size());
    memcpy(&datagram[0], nonce.data(), nonce.size());
    if (!CryptAesCtr(packet->payload.data(), packet->payload.size(),
                     reinterpret_cast<const uint8_t*>(nonce.data()),
                     reinterpret_cast<uint8_t*>(&datagram[nonce.size()]))) {
        C1XZ_LOGE(TAG, "failed to encrypt an audio packet");
        return false;
    }

    // Sent outside channel_mutex_: the receive thread needs that lock too.
    return udp->Send(datagram.data(), datagram.size());
}

void MqttProtocol::HandleUdpDatagram(const char* data, size_t len) {
    if (len < kAudioHeaderSize) {
        C1XZ_LOGE(TAG, "audio datagram of %zu bytes is shorter than the header", len);
        return;
    }
    if (static_cast<uint8_t>(data[0]) != 0x01) {
        C1XZ_LOGE(TAG, "unexpected audio packet type 0x%02x", static_cast<uint8_t>(data[0]));
        return;
    }

    uint16_t payload_len = 0;
    uint32_t timestamp = 0;
    uint32_t sequence = 0;
    memcpy(&payload_len, data + 2, sizeof(payload_len));
    memcpy(&timestamp, data + 8, sizeof(timestamp));
    memcpy(&sequence, data + 12, sizeof(sequence));
    payload_len = ntohs(payload_len);
    timestamp = ntohl(timestamp);
    sequence = ntohl(sequence);

    if (len != kAudioHeaderSize + payload_len) {
        C1XZ_LOGE(TAG, "audio length mismatch: header says %u, datagram carries %zu", payload_len,
                  len - kAudioHeaderSize);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (sequence <= remote_sequence_) {
            C1XZ_LOGW(TAG, "dropping replayed or reordered packet %u (last %u)", sequence,
                      remote_sequence_);
            return;
        }
        if (sequence != remote_sequence_ + 1) {
            C1XZ_LOGW(TAG, "packet gap: got %u, expected %u", sequence, remote_sequence_ + 1);
        }
    }

    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = server_sample_rate_;
    packet->frame_duration = server_frame_duration_;
    packet->timestamp = timestamp;
    packet->payload.resize(payload_len);
    if (!CryptAesCtr(reinterpret_cast<const uint8_t*>(data + kAudioHeaderSize), payload_len,
                     reinterpret_cast<const uint8_t*>(data), packet->payload.data())) {
        C1XZ_LOGE(TAG, "failed to decrypt an audio packet");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (sequence <= remote_sequence_) {
            return;
        }
        remote_sequence_ = sequence;
    }
    last_incoming_time_ = std::chrono::steady_clock::now();
    if (on_incoming_audio_ != nullptr) {
        on_incoming_audio_(std::move(packet));
    }
}

bool MqttProtocol::OpenAudioChannel() {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
            C1XZ_LOGI(TAG, "MQTT is down, reconnecting before opening the audio channel");
        }
    }
    bool connected;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        connected = mqtt_ != nullptr && mqtt_->IsConnected();
    }
    if (!connected && !StartMqttClient(true)) {
        return false;
    }

    error_occurred_ = false;
    {
        std::lock_guard<std::mutex> lock(hello_mutex_);
        server_hello_received_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        local_sequence_ = 0;
        remote_sequence_ = 0;
    }

    if (!SendText(GetHelloMessage())) {
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(hello_mutex_);
        if (!hello_cv_.wait_for(lock, std::chrono::seconds(10),
                                [this] { return server_hello_received_; })) {
            C1XZ_LOGE(TAG, "server did not answer the hello within 10s");
            SetError("服务器无响应");
            return false;
        }
    }

    auto udp = std::make_shared<c1xz::UdpSocket>();
    udp->OnMessage([this](const char* data, size_t len) { HandleUdpDatagram(data, len); });

    std::string server;
    int port;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        server = udp_server_;
        port = udp_port_;
    }
    if (!udp->Connect(server, port)) {
        SetError("无法连接语音通道", udp->error());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_ = std::move(udp);
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void MqttProtocol::CloseAudioChannel(bool send_goodbye) {
    std::shared_ptr<c1xz::UdpSocket> udp;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp = std::move(udp_);
        udp_.reset();
    }
    if (udp != nullptr) {
        udp->Close();
    }

    if (send_goodbye && !session_id_.empty()) {
        std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"goodbye\"}";
        SendText(message);
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::IsAudioChannelOpened() const {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    return udp_ != nullptr && mqtt_ != nullptr && mqtt_->IsConnected() && !error_occurred_ &&
           !IsTimeout();
}

std::string MqttProtocol::GetHelloMessage() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);

    char* text = cJSON_PrintUnformatted(root);
    std::string message(text != nullptr ? text : "{}");
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    cJSON* transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "udp") != 0) {
        C1XZ_LOGE(TAG, "server hello did not offer the udp transport");
        return;
    }

    cJSON* session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
    }

    cJSON* audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        cJSON* sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        cJSON* frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    cJSON* udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        C1XZ_LOGE(TAG, "server hello has no udp section");
        return;
    }
    cJSON* server = cJSON_GetObjectItem(udp, "server");
    cJSON* port = cJSON_GetObjectItem(udp, "port");
    cJSON* key_item = cJSON_GetObjectItem(udp, "key");
    cJSON* nonce_item = cJSON_GetObjectItem(udp, "nonce");
    if (!cJSON_IsString(server) || !cJSON_IsNumber(port) || port->valueint <= 0 ||
        port->valueint > UINT16_MAX || !cJSON_IsString(key_item) || !cJSON_IsString(nonce_item)) {
        C1XZ_LOGE(TAG, "udp section is missing server, port, key or nonce");
        return;
    }

    std::string aes_nonce;
    std::string aes_key;
    if (!DecodeHexString(nonce_item->valuestring, &aes_nonce) ||
        !DecodeHexString(key_item->valuestring, &aes_key) || aes_nonce.size() != 16 ||
        aes_key.size() != 16) {
        C1XZ_LOGE(TAG, "AES key or nonce is not 16 bytes of hex");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        mbedtls_aes_free(&aes_ctx_);
        mbedtls_aes_init(&aes_ctx_);
        // CTR mode only ever uses the forward transform, for both directions.
        int rc = mbedtls_aes_setkey_enc(&aes_ctx_,
                                        reinterpret_cast<const unsigned char*>(aes_key.data()),
                                        128);
        if (rc != 0) {
            C1XZ_LOGE(TAG, "failed to install the AES key: -0x%04x", -rc);
            aes_ready_ = false;
            return;
        }
        aes_ready_ = true;
    }

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_server_ = server->valuestring;
        udp_port_ = port->valueint;
        aes_nonce_ = aes_nonce;
    }

    {
        std::lock_guard<std::mutex> lock(hello_mutex_);
        server_hello_received_ = true;
    }
    hello_cv_.notify_all();
}
