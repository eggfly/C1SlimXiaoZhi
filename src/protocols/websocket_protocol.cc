#include "protocols/websocket_protocol.h"

#include <arpa/inet.h>
#include <cstring>

#include "audio/audio_service.h"
#include "boards/board.h"
#include "platform/log.h"
#include "settings.h"
#include "system_info.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() = default;

WebsocketProtocol::~WebsocketProtocol() { CloseAudioChannel(false); }

bool WebsocketProtocol::Start() {
    // The channel is opened lazily, when a conversation starts.
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto* bp2 = reinterpret_cast<BinaryProtocol2*>(&serialized[0]);
        bp2->version = htons(static_cast<uint16_t>(version_));
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(static_cast<uint32_t>(packet->payload.size()));
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());
        return websocket_->Send(serialized.data(), serialized.size(), true);
    }
    if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto* bp3 = reinterpret_cast<BinaryProtocol3*>(&serialized[0]);
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(static_cast<uint16_t>(packet->payload.size()));
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());
        return websocket_->Send(serialized.data(), serialized.size(), true);
    }
    return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    if (!websocket_->SendText(text)) {
        C1XZ_LOGE(TAG, "failed to send: %s", text.c_str());
        SetError("服务器连接中断");
        return false;
    }
    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // The websocket transport has no goodbye message.
    if (websocket_ != nullptr) {
        websocket_->Close();
        websocket_.reset();
    }
}

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }
    if (url.empty()) {
        C1XZ_LOGE(TAG, "no websocket URL configured; the OTA check must run first");
        SetError("尚未获取服务器地址");
        return false;
    }

    error_occurred_ = false;
    {
        std::lock_guard<std::mutex> lock(hello_mutex_);
        server_hello_received_ = false;
    }

    websocket_.reset(new c1xz::WebSocketClient());
    if (!token.empty()) {
        if (token.find(' ') == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token);
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_));
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (on_incoming_audio_ == nullptr) {
                return;
            }
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = server_sample_rate_;
            packet->frame_duration = server_frame_duration_;

            if (version_ == 2 && len >= sizeof(BinaryProtocol2)) {
                const auto* bp2 = reinterpret_cast<const BinaryProtocol2*>(data);
                uint32_t size = ntohl(bp2->payload_size);
                if (sizeof(BinaryProtocol2) + size > len) {
                    C1XZ_LOGE(TAG, "binary v2 frame claims %u payload bytes but only %zu arrived",
                              size, len - sizeof(BinaryProtocol2));
                    return;
                }
                packet->timestamp = ntohl(bp2->timestamp);
                packet->payload.assign(bp2->payload, bp2->payload + size);
            } else if (version_ == 3 && len >= sizeof(BinaryProtocol3)) {
                const auto* bp3 = reinterpret_cast<const BinaryProtocol3*>(data);
                uint16_t size = ntohs(bp3->payload_size);
                if (sizeof(BinaryProtocol3) + size > len) {
                    C1XZ_LOGE(TAG, "binary v3 frame claims %u payload bytes but only %zu arrived",
                              size, len - sizeof(BinaryProtocol3));
                    return;
                }
                packet->payload.assign(bp3->payload, bp3->payload + size);
            } else if (version_ == 1) {
                packet->payload.assign(reinterpret_cast<const uint8_t*>(data),
                                       reinterpret_cast<const uint8_t*>(data) + len);
            } else {
                C1XZ_LOGE(TAG, "binary frame too short for protocol version %d", version_);
                return;
            }
            on_incoming_audio_(std::move(packet));
        } else {
            cJSON* root = cJSON_ParseWithLength(data, len);
            if (root == nullptr) {
                C1XZ_LOGE(TAG, "unparseable JSON message of %zu bytes", len);
                return;
            }
            cJSON* type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else if (on_incoming_json_ != nullptr) {
                    on_incoming_json_(root);
                }
            } else {
                C1XZ_LOGE(TAG, "message without a type: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        C1XZ_LOGI(TAG, "websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    C1XZ_LOGI(TAG, "connecting to %s (protocol version %d)", url.c_str(), version_);
    if (!websocket_->Connect(url)) {
        SetError("无法连接服务器", url);
        return false;
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

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);

    cJSON* features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", true);
    // Server-side AEC needs the timestamped binary protocol to line the uplink
    // up with playback, so only claim it when that protocol is selected.
    if (version_ == 2) {
        cJSON_AddBoolToObject(features, "aec", true);
    }
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);

    cJSON_AddStringToObject(root, "transport", "websocket");
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

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    cJSON* transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport)) {
        C1XZ_LOGE(TAG, "server hello has no transport field");
        return;
    }
    if (strcmp(transport->valuestring, "websocket") != 0) {
        C1XZ_LOGE(TAG, "server asked for transport %s, which this build does not speak",
                  transport->valuestring);
        return;
    }

    cJSON* session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        C1XZ_LOGI(TAG, "session %s", session_id_.c_str());
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

    {
        std::lock_guard<std::mutex> lock(hello_mutex_);
        server_hello_received_ = true;
    }
    hello_cv_.notify_all();
}
