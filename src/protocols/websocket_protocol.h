#ifndef C1XZ_PROTOCOLS_WEBSOCKET_PROTOCOL_H
#define C1XZ_PROTOCOLS_WEBSOCKET_PROTOCOL_H

#include <condition_variable>
#include <memory>
#include <mutex>

#include "net/websocket_client.h"
#include "protocols/protocol.h"

// Ported from xiaozhi-esp32 main/protocols/websocket_protocol.*, with the
// FreeRTOS event group replaced by a condition variable and the IDF websocket
// client replaced by c1xz::WebSocketClient.
class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol() override;

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    std::unique_ptr<c1xz::WebSocketClient> websocket_;
    int version_ = 1;

    std::mutex hello_mutex_;
    std::condition_variable hello_cv_;
    bool server_hello_received_ = false;

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif  // C1XZ_PROTOCOLS_WEBSOCKET_PROTOCOL_H
