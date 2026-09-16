#include "application.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "display/display.h"
#include "mcp_server.h"
#include "platform/log.h"
#include "protocols/mqtt_protocol.h"
#include "protocols/websocket_protocol.h"
#include "settings.h"
#include "system_info.h"

#define TAG "App"

namespace {

std::string EnvOr(const char* name, const char* fallback) {
    const char* value = getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

std::string ReadWholeFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return {};
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string data;
    if (size > 0 && size < 4 * 1024 * 1024) {
        data.resize(static_cast<size_t>(size));
        if (fread(&data[0], 1, data.size(), f) != data.size()) {
            data.clear();
        }
    }
    fclose(f);
    return data;
}

}  // namespace

Application& Application::GetInstance() {
    static Application instance;
    return instance;
}

Application::Application() = default;

Application::~Application() = default;

void Application::Schedule(std::function<void()> task) { loop_.Post(std::move(task)); }

const char* Application::StatusTextFor(DeviceState state) const {
    switch (state) {
        case kDeviceStateStarting: return "正在启动";
        case kDeviceStateWifiConfiguring: return "等待网络";
        case kDeviceStateIdle: return "待命";
        case kDeviceStateConnecting: return "连接中";
        case kDeviceStateListening: return "聆听中";
        case kDeviceStateSpeaking: return "说话中";
        case kDeviceStateNotifying: return "通知";
        case kDeviceStateUpgrading: return "升级中";
        case kDeviceStateActivating: return "激活中";
        case kDeviceStateAudioTesting: return "音频测试";
        case kDeviceStateFatalError: return "发生错误";
        default: return "";
    }
}

bool Application::Start() {
    auto& board = Board::GetInstance();
    if (!board.Initialize()) {
        C1XZ_LOGE(TAG, "board initialisation failed");
        return false;
    }

    Display* display = board.GetDisplay();
    state_machine_.AddStateChangeListener([this](DeviceState previous, DeviceState current) {
        OnStateChanged(previous, current);
    });
    SetDeviceState(kDeviceStateStarting);

    if (!audio_service_.Initialize(board.GetAudioCodec())) {
        C1XZ_LOGE(TAG, "audio service failed to start");
        return false;
    }

    Settings audio_settings("audio", false);
    audio_service_.SetOutputVolume(audio_settings.GetInt("volume", 70));
    audio_service_.SetVadThresholdDb(audio_settings.GetInt("vad_threshold_db", 45));

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        Schedule([this]() { PumpSendQueue(); });
    };
    callbacks.on_vad_change = [this](bool speaking) {
        Schedule([this, speaking]() {
            voice_detected_ = speaking;
            // In auto mode the server decides when the turn ends, but a local
            // VAD still drives the on-screen indicator.
            UpdateStatusBar();
        });
    };
    callbacks.on_playback_drained = [this]() {
        Schedule([this]() {
            if (GetDeviceState() == kDeviceStateSpeaking && !keep_listening_) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    };
    audio_service_.SetCallbacks(callbacks);
    audio_service_.Start();

    sound_directory_ = EnvOr("C1XZ_SOUNDS", "/storage/c1/xiaozhi/sounds");

    board.SetKeyHandler([this](const Board::KeyEvent& event) {
        Schedule([this, event]() { OnKeyEvent(event); });
    });

    ota_.reset(new Ota());

    McpServer::GetInstance().SetSendCallback([this](const std::string& payload) {
        if (protocol_ != nullptr) {
            protocol_->SendMcpMessage(payload);
        }
    });
    McpServer::GetInstance().RegisterCommonTools();

    // Status bar refresh. Slow on purpose: the panel needs 685 ms per frame and
    // the display layer drops identical frames anyway.
    status_timer_ = loop_.PostRepeating(std::chrono::seconds(10), [this]() { UpdateStatusBar(); });

    if (display != nullptr) {
        display->SetStatus(StatusTextFor(kDeviceStateStarting));
    }
    UpdateStatusBar();

    std::thread([this]() { StartupSequence(); }).detach();
    return true;
}

void Application::Run() { loop_.Run(); }

void Application::Quit() {
    if (quitting_.exchange(true)) {
        return;
    }
    C1XZ_LOGI(TAG, "shutting down");
    if (protocol_ != nullptr) {
        protocol_->CloseAudioChannel(true);
    }
    audio_service_.Stop();
    Settings::Flush();
    Board::GetInstance().Shutdown();
    loop_.Stop();
}

void Application::StartupSequence() {
    auto& board = Board::GetInstance();

    // Wait for the network. The radio is owned by the launcher or the factory
    // scripts, so we watch rather than configure.
    for (int i = 0; i < 60 && !board.IsNetworkReady() && !quitting_.load(); ++i) {
        if (i == 0) {
            Schedule([this]() { SetDeviceState(kDeviceStateWifiConfiguring); });
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (quitting_.load()) {
        return;
    }
    if (!board.IsNetworkReady()) {
        Schedule([this]() {
            Alert("网络", "未连接 Wi-Fi，请先在系统设置里联网。", "cloud_off", "exclamation");
        });
        return;
    }

    Schedule([this]() { SetDeviceState(kDeviceStateActivating); });
    CheckNewVersion();
    if (quitting_.load()) {
        return;
    }

    Schedule([this]() {
        DismissAlert();
        CreateProtocol();
        SetDeviceState(kDeviceStateIdle);
    });
}

void Application::CheckNewVersion() {
    const int kMaxRetries = 10;
    int retries = 0;
    int delay_seconds = 10;
    auto& board = Board::GetInstance();

    while (!quitting_.load()) {
        Display* display = board.GetDisplay();
        if (display != nullptr) {
            display->SetStatus("检查更新");
        }

        Ota::Result result = ota_->CheckVersion();
        if (result != Ota::Result::kOk) {
            if (++retries >= kMaxRetries) {
                C1XZ_LOGE(TAG, "giving up on the version check");
                Schedule([this]() {
                    Alert("网络", "无法连接服务器，请检查网络后重试。", "cloud_off",
                          "exclamation");
                });
                return;
            }
            std::string detail = ota_->GetLastError();
            Schedule([this, delay_seconds, detail]() {
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "检查更新失败，%d 秒后重试。\n%s", delay_seconds,
                         detail.c_str());
                Alert("错误", buffer, "cloud_off", "exclamation");
            });
            for (int i = 0; i < delay_seconds && !quitting_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            delay_seconds *= 2;
            continue;
        }
        retries = 0;
        delay_seconds = 10;

        if (ota_->HasNewVersion()) {
            Schedule([this]() {
                SetDeviceState(kDeviceStateUpgrading);
                Alert("升级", "正在下载新版本…", "cloud_download", "");
            });
            // Returns only on failure; on success the process is replaced.
            ota_->StartUpgrade([this](int percent, size_t) {
                Schedule([this, percent]() {
                    Display* display = Board::GetInstance().GetDisplay();
                    if (display != nullptr) {
                        display->SetStatus("升级 " + std::to_string(percent) + "%");
                    }
                });
            });
            Schedule([this]() {
                DismissAlert();
                SetDeviceState(kDeviceStateActivating);
            });
        }

        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            return;  // nothing left to do
        }

        if (ota_->HasActivationCode()) {
            std::string code = ota_->GetActivationCode();
            std::string message = ota_->GetActivationMessage();
            Schedule([this, code, message]() { ShowActivationCode(code, message); });
        }

        // Poll until the user finishes entering the code on the website.
        for (int i = 0; i < 10 && !quitting_.load(); ++i) {
            Ota::ActivationResult activation = ota_->Activate();
            if (activation == Ota::ActivationResult::kActivated) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(
                activation == Ota::ActivationResult::kPending ? 3 : 10));
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    std::string body = message;
    if (!body.empty()) {
        body += "\n\n";
    }
    body += "验证码：" + code;
    Alert("激活设备", body, "link", "activation");
    PlaySound("activation");
    C1XZ_LOGI(TAG, "activation code: %s", code.c_str());
}

void Application::CreateProtocol() {
    Settings websocket_settings("websocket", false);
    Settings mqtt_settings("mqtt", false);
    bool has_websocket = !websocket_settings.GetString("url").empty();
    bool has_mqtt = !mqtt_settings.GetString("endpoint").empty();

    // Same preference as upstream: websocket when the server offered one,
    // otherwise MQTT+UDP.
    if (has_websocket) {
        protocol_.reset(new WebsocketProtocol());
    } else if (has_mqtt) {
        protocol_.reset(new MqttProtocol());
    } else {
        C1XZ_LOGE(TAG, "the server gave us no transport configuration");
        Alert("错误", "服务器未下发连接信息。", "cloud_off", "exclamation");
        return;
    }

    protocol_->OnIncomingJson([this](const cJSON* root) {
        // Runs on the receive thread; copy what we need and hand it over.
        char* text = cJSON_PrintUnformatted(root);
        if (text == nullptr) {
            return;
        }
        std::string payload(text);
        cJSON_free(text);
        Schedule([this, payload]() {
            cJSON* copy = cJSON_Parse(payload.c_str());
            if (copy != nullptr) {
                OnIncomingJson(copy);
                cJSON_Delete(copy);
            }
        });
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        // Straight to the decode queue: routing this through the event loop
        // would add latency to every 60 ms of speech for no benefit.
        if (GetDeviceState() == kDeviceStateSpeaking ||
            GetDeviceState() == kDeviceStateListening) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this]() { Schedule([this]() { OnAudioChannelOpened(); }); });
    protocol_->OnAudioChannelClosed([this]() { Schedule([this]() { OnAudioChannelClosed(); }); });
    protocol_->OnNetworkError([this](const std::string& message) {
        Schedule([this, message]() { OnNetworkError(message); });
    });

    protocol_->Start();
}

void Application::SetDeviceState(DeviceState state) { state_machine_.TransitionTo(state); }

void Application::OnStateChanged(DeviceState previous, DeviceState current) {
    Display* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetStatus(StatusTextFor(current));
    }

    switch (current) {
        case kDeviceStateIdle:
            audio_service_.EnableVoiceProcessing(false);
            if (send_timer_ != 0) {
                loop_.CancelTimer(send_timer_);
                send_timer_ = 0;
            }
            break;
        case kDeviceStateListening:
            audio_service_.ResetDecoder();
            audio_service_.EnableVoiceProcessing(true);
            if (send_timer_ == 0) {
                // Belt and braces: the audio callback already nudges the pump,
                // but a timer guarantees the queue drains even if a callback is
                // lost during a state change.
                send_timer_ = loop_.PostRepeating(std::chrono::milliseconds(20),
                                                  [this]() { PumpSendQueue(); });
            }
            break;
        case kDeviceStateSpeaking:
            // Half duplex: without echo cancellation the microphone would hear
            // the speaker and the server would transcribe our own voice.
            audio_service_.EnableVoiceProcessing(false);
            break;
        default:
            break;
    }

    if (previous == kDeviceStateSpeaking && current == kDeviceStateIdle) {
        audio_service_.ResetDecoder();
    }
    UpdateStatusBar();
}

void Application::PumpSendQueue() {
    if (protocol_ == nullptr || !protocol_->IsAudioChannelOpened()) {
        return;
    }
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }
    while (auto packet = audio_service_.PopPacketFromSendQueue()) {
        if (!protocol_->SendAudio(std::move(packet))) {
            break;
        }
    }
}

void Application::OnAudioChannelOpened() {
    C1XZ_LOGI(TAG, "audio channel open");
    has_server_hello_ = true;
    aborted_ = false;
    DismissAlert();

    Display* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ClearChatMessages();
    }

    SetDeviceState(kDeviceStateListening);
    protocol_->SendStartListening(listening_mode_);
}

void Application::OnAudioChannelClosed() {
    C1XZ_LOGI(TAG, "audio channel closed");
    has_server_hello_ = false;
    keep_listening_ = false;
    audio_service_.EnableVoiceProcessing(false);
    if (GetDeviceState() != kDeviceStateIdle) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::OnNetworkError(const std::string& message) {
    C1XZ_LOGE(TAG, "network error: %s", message.c_str());
    keep_listening_ = false;
    if (GetDeviceState() != kDeviceStateIdle) {
        SetDeviceState(kDeviceStateIdle);
    }
    Alert("错误", message, "cloud_off", "exclamation");
}

void Application::OnIncomingJson(const cJSON* root) {
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        return;
    }
    const char* name = type->valuestring;
    Display* display = Board::GetInstance().GetDisplay();

    if (strcmp(name, "tts") == 0) {
        const cJSON* state = cJSON_GetObjectItem(root, "state");
        if (!cJSON_IsString(state)) {
            return;
        }
        if (strcmp(state->valuestring, "start") == 0) {
            aborted_ = false;
            if (GetDeviceState() == kDeviceStateIdle ||
                GetDeviceState() == kDeviceStateListening) {
                SetDeviceState(kDeviceStateSpeaking);
            }
        } else if (strcmp(state->valuestring, "stop") == 0) {
            if (GetDeviceState() == kDeviceStateSpeaking) {
                if (keep_listening_) {
                    protocol_->SendStartListening(listening_mode_);
                    SetDeviceState(kDeviceStateListening);
                } else {
                    // Wait for the playback queue to drain before going idle,
                    // otherwise the tail of the answer is cut off.
                    if (audio_service_.IsPlaybackIdle()) {
                        SetDeviceState(kDeviceStateIdle);
                    }
                }
            }
        } else if (strcmp(state->valuestring, "sentence_start") == 0) {
            const cJSON* text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && display != nullptr) {
                display->SetChatMessage("assistant", text->valuestring);
            }
        }
        return;
    }

    if (strcmp(name, "stt") == 0) {
        const cJSON* text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && display != nullptr) {
            display->SetChatMessage("user", text->valuestring);
            C1XZ_LOGI(TAG, "user said: %s", text->valuestring);
        }
        return;
    }

    if (strcmp(name, "llm") == 0) {
        const cJSON* emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emotion) && display != nullptr) {
            display->SetEmotion(emotion->valuestring);
        }
        return;
    }

    if (strcmp(name, "mcp") == 0) {
        const cJSON* payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload)) {
            McpServer::GetInstance().ParseMessage(payload);
        }
        return;
    }

    if (strcmp(name, "system") == 0) {
        const cJSON* command = cJSON_GetObjectItem(root, "command");
        if (cJSON_IsString(command) && strcmp(command->valuestring, "reboot") == 0) {
            Board::GetInstance().Reboot();
        }
        return;
    }

    if (strcmp(name, "alert") == 0) {
        const cJSON* status = cJSON_GetObjectItem(root, "status");
        const cJSON* message = cJSON_GetObjectItem(root, "message");
        const cJSON* emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(status) && cJSON_IsString(message)) {
            Alert(status->valuestring, message->valuestring,
                  cJSON_IsString(emotion) ? emotion->valuestring : "", "exclamation");
        }
        return;
    }

    if (strcmp(name, "goodbye") == 0) {
        protocol_->CloseAudioChannel(false);
        return;
    }

    C1XZ_LOGD(TAG, "ignoring message type %s", name);
}

void Application::ToggleChatState() {
    DeviceState state = GetDeviceState();
    if (state == kDeviceStateActivating || state == kDeviceStateUpgrading ||
        state == kDeviceStateStarting) {
        return;
    }
    if (protocol_ == nullptr) {
        Alert("提示", "尚未连接服务器。", "cloud_off", "exclamation");
        return;
    }

    if (state == kDeviceStateIdle) {
        SetDeviceState(kDeviceStateConnecting);
        keep_listening_ = listening_mode_ == kListeningModeAutoStop;
        // Opening the channel blocks on the network; keep it off the loop.
        std::thread([this]() {
            if (!protocol_->OpenAudioChannel()) {
                Schedule([this]() { SetDeviceState(kDeviceStateIdle); });
            }
        }).detach();
        return;
    }

    if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        return;
    }

    if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel(true);
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::StartListening() {
    if (protocol_ == nullptr) {
        return;
    }
    DeviceState state = GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    }
    if (state == kDeviceStateIdle) {
        keep_listening_ = false;
        listening_mode_ = kListeningModeManualStop;
        SetDeviceState(kDeviceStateConnecting);
        std::thread([this]() {
            if (!protocol_->OpenAudioChannel()) {
                Schedule([this]() { SetDeviceState(kDeviceStateIdle); });
            }
        }).detach();
    } else if (state == kDeviceStateListening) {
        // Already listening; nothing to do.
    }
}

void Application::StopListening() {
    if (protocol_ == nullptr || GetDeviceState() != kDeviceStateListening) {
        return;
    }
    protocol_->SendStopListening();
    audio_service_.EnableVoiceProcessing(false);
}

void Application::AbortSpeaking(AbortReason reason) {
    if (protocol_ == nullptr || aborted_) {
        return;
    }
    aborted_ = true;
    C1XZ_LOGI(TAG, "aborting the current response");
    protocol_->SendAbortSpeaking(reason);
    audio_service_.ClearQueues();
    audio_service_.ResetDecoder();
    if (GetDeviceState() == kDeviceStateSpeaking) {
        SetDeviceState(keep_listening_ ? kDeviceStateListening : kDeviceStateIdle);
    }
}

void Application::Alert(const std::string& status, const std::string& message,
                        const std::string& emotion, const std::string& sound) {
    C1XZ_LOGW(TAG, "alert [%s] %s", status.c_str(), message.c_str());
    Display* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowAlert(status, message);
        if (!emotion.empty()) {
            display->SetEmotion(emotion);
        }
    }
    if (!sound.empty()) {
        PlaySound(sound);
    }
}

void Application::DismissAlert() {
    Display* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->DismissAlert();
    }
}

void Application::PlaySound(const std::string& name) {
    if (sound_directory_.empty()) {
        return;
    }
    std::string path = sound_directory_ + "/" + name + ".ogg";
    std::string data = ReadWholeFile(path);
    if (data.empty()) {
        // Prompts are optional: the device is perfectly usable without them,
        // and they are not shipped in the binary because of their size.
        C1XZ_LOGD(TAG, "no prompt sound at %s", path.c_str());
        return;
    }
    audio_service_.PlaySound(data);
}

void Application::AdjustVolume(int delta) {
    int volume = audio_service_.output_volume() + delta;
    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    audio_service_.SetOutputVolume(volume);
    Settings settings("audio", true);
    settings.SetInt("volume", volume);

    Display* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification("音量 " + std::to_string(volume) + "%", 2000);
    }
}

void Application::OnKeyEvent(const Board::KeyEvent& event) {
    if (!event.pressed) {
        // Push to talk: releasing the talk key ends the turn in manual mode.
        if (event.key == Board::Key::kTalk &&
            listening_mode_ == kListeningModeManualStop &&
            GetDeviceState() == kDeviceStateListening) {
            StopListening();
        }
        return;
    }

    switch (event.key) {
        case Board::Key::kTalk:
            ToggleChatState();
            break;
        case Board::Key::kCancel:
            if (GetDeviceState() == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonNone);
            } else {
                DismissAlert();
            }
            break;
        case Board::Key::kExit:
            Quit();
            break;
        case Board::Key::kVolumeUp:
            AdjustVolume(+10);
            break;
        case Board::Key::kVolumeDown:
            AdjustVolume(-10);
            break;
        case Board::Key::kMode: {
            listening_mode_ = listening_mode_ == kListeningModeAutoStop
                                  ? kListeningModeManualStop
                                  : kListeningModeAutoStop;
            Display* display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->ShowNotification(
                    listening_mode_ == kListeningModeAutoStop ? "自动对话" : "按住说话", 2000);
            }
            break;
        }
        default:
            break;
    }
}

void Application::UpdateStatusBar() {
    auto& board = Board::GetInstance();
    Display* display = board.GetDisplay();
    if (display == nullptr) {
        return;
    }
    display->SetNetworkState(board.IsNetworkReady(), board.GetNetworkRssi());

    int percent = 0;
    bool charging = false;
    if (board.GetBatteryLevel(&percent, &charging)) {
        display->SetBatteryState(percent, charging);
        if (percent <= 10 && !charging) {
            display->ShowNotification("电量不足", 5000);
            PlaySound("low_battery");
        }
    }
    display->SetListeningMode(listening_mode_ == kListeningModeAutoStop ? "自动" : "手动");
}
