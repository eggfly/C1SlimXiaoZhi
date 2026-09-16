#ifndef C1XZ_PROTOCOLS_PROTOCOL_H
#define C1XZ_PROTOCOLS_PROTOCOL_H

// Ported from xiaozhi-esp32 main/protocols/protocol.h. The message contract is
// shared with the ESP32 firmware and must not drift: see docs/websocket.md and
// docs/mqtt-udp.md upstream.

#include <cJSON.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    uint32_t playback_id = 0;
    uint32_t media_position_ms = 0;
    std::vector<uint8_t> payload;
};

// Binary protocol version 2: carries a timestamp so the server can run AEC.
struct BinaryProtocol2 {
    uint16_t version;
    uint16_t type;  // 0: OPUS, 1: JSON
    uint32_t reserved;
    uint32_t timestamp;     // milliseconds
    uint32_t payload_size;  // bytes
    uint8_t payload[];
} __attribute__((packed));

// Binary protocol version 3: compact header, no timestamp.
struct BinaryProtocol3 {
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size;
    uint8_t payload[];
} __attribute__((packed));

enum AbortReason { kAbortReasonNone, kAbortReasonWakeWordDetected };

enum ListeningMode {
    kListeningModeAutoStop,
    kListeningModeManualStop,
    kListeningModeRealtime,  // needs AEC
};

class Protocol {
public:
    virtual ~Protocol() = default;

    inline int server_sample_rate() const { return server_sample_rate_; }
    inline int server_frame_duration() const { return server_frame_duration_; }
    inline const std::string& session_id() const { return session_id_; }

    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);
    void OnAudioChannelOpened(std::function<void()> callback);
    void OnAudioChannelClosed(std::function<void()> callback);
    void OnNetworkError(std::function<void(const std::string& message)> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);

    virtual bool Start() = 0;
    virtual bool OpenAudioChannel() = 0;
    virtual void CloseAudioChannel(bool send_goodbye = true) = 0;
    virtual bool IsAudioChannelOpened() const = 0;
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;
    virtual void SendWakeWordDetected(const std::string& wake_word);
    virtual void SendStartListening(ListeningMode mode);
    virtual void SendStopListening();
    virtual void SendAbortSpeaking(AbortReason reason);
    virtual void SendMcpMessage(const std::string& message);

protected:
    std::function<void(const cJSON* root)> on_incoming_json_;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string& message)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    bool error_occurred_ = false;
    std::string session_id_;
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;

    virtual bool SendText(const std::string& text) = 0;
    virtual void SetError(const std::string& message);
    void SetError(const std::string& message, const std::string& detail);
    virtual bool IsTimeout() const;

    // Declares the dynamic glyph push extension. This port renders text from a
    // locally compiled bitmap font, so it advertises glyph_push:false and the
    // server keeps sending plain text.
    static void AddTextFontCapabilities(cJSON* root);
};

#endif  // C1XZ_PROTOCOLS_PROTOCOL_H
