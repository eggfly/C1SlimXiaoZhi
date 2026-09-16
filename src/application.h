#ifndef C1XZ_APPLICATION_H
#define C1XZ_APPLICATION_H

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "audio/audio_service.h"
#include "boards/board.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "ota.h"
#include "platform/event_loop.h"
#include "protocols/protocol.h"

// Ported from xiaozhi-esp32 main/application.*.
//
// Owns the conversation: the state machine, the protocol lifecycle, the audio
// pump and everything the user sees. All application state is touched from the
// event loop thread only; audio and network threads hand work over with
// Schedule().
class Application {
public:
    static Application& GetInstance();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool Start();
    void Run();
    void Quit();

    // Runs `task` on the event loop thread. Safe from any thread.
    void Schedule(std::function<void()> task);

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    void SetDeviceState(DeviceState state);

    // User actions.
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void AbortSpeaking(AbortReason reason);

    void Alert(const std::string& status, const std::string& message,
               const std::string& emotion = std::string(),
               const std::string& sound = std::string());
    void DismissAlert();

    AudioService& audio_service() { return audio_service_; }

private:
    Application();
    ~Application();

    c1xz::EventLoop loop_;
    DeviceStateMachine state_machine_;
    AudioService audio_service_;
    std::unique_ptr<Protocol> protocol_;
    std::unique_ptr<Ota> ota_;

    std::atomic<bool> quitting_{false};
    bool voice_detected_ = false;
    bool aborted_ = false;
    bool has_server_hello_ = false;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    bool keep_listening_ = false;
    std::string sound_directory_;
    int64_t last_activity_ms_ = 0;
    c1xz::EventLoop::TimerId status_timer_ = 0;
    c1xz::EventLoop::TimerId send_timer_ = 0;

    // Startup work that needs the network: version check, activation, then the
    // first connection. Runs on its own thread so the event loop stays live and
    // the screen keeps updating.
    void StartupSequence();
    void CheckNewVersion();
    void ShowActivationCode(const std::string& code, const std::string& message);

    void CreateProtocol();
    void OnIncomingJson(const cJSON* root);
    void OnIncomingAudio(std::unique_ptr<AudioStreamPacket> packet);
    void OnAudioChannelOpened();
    void OnAudioChannelClosed();
    void OnNetworkError(const std::string& message);

    void OnKeyEvent(const Board::KeyEvent& event);
    void OnStateChanged(DeviceState previous, DeviceState current);

    void PumpSendQueue();
    void UpdateStatusBar();
    void PlaySound(const std::string& name);
    void AdjustVolume(int delta);

    const char* StatusTextFor(DeviceState state) const;
};

#endif  // C1XZ_APPLICATION_H
