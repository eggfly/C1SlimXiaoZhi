#ifndef C1XZ_DISPLAY_DISPLAY_H
#define C1XZ_DISPLAY_DISPLAY_H

#include <deque>
#include <mutex>
#include <string>

// Display interface, in the shape of the upstream Display class so the ported
// application logic calls it unchanged. The concrete implementation on the
// C1 Slim paints a 296x152 one-bit framebuffer; see EpaperDisplay.
class Display {
public:
    Display() = default;
    virtual ~Display() = default;

    virtual bool Initialize() { return true; }
    virtual void Shutdown() {}

    // Status line text, e.g. "待命", "聆听中", "说话中".
    virtual void SetStatus(const std::string& status);

    // Transient banner. Replaces the status area until it expires.
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);

    // Emotion name from the server's llm message ("happy", "thinking", ...).
    virtual void SetEmotion(const std::string& emotion);

    // Appends a line of conversation. `role` is "user" or "assistant".
    virtual void SetChatMessage(const std::string& role, const std::string& content);
    virtual void ClearChatMessages();

    // A full-screen modal used for errors and for the activation code.
    virtual void ShowAlert(const std::string& title, const std::string& message);
    virtual void DismissAlert();

    // Redraws whatever changed. Implementations coalesce and rate limit; on
    // e-paper a frame costs 685 ms, so callers may call this freely.
    virtual void Update() {}

    virtual void SetPowerSaveMode(bool enable) { (void)enable; }

    virtual int width() const { return 0; }
    virtual int height() const { return 0; }
    virtual bool IsMonochrome() const { return true; }

    // Status bar inputs, set by the board.
    void SetNetworkState(bool connected, int rssi);
    void SetBatteryState(int percent, bool charging);
    void SetMuted(bool muted);
    void SetListeningMode(const std::string& mode);

protected:
    struct ChatLine {
        std::string role;
        std::string content;
    };

    mutable std::mutex mutex_;
    std::string status_;
    std::string notification_;
    int64_t notification_expires_ms_ = 0;
    std::string emotion_;
    std::deque<ChatLine> chat_;
    std::string alert_title_;
    std::string alert_message_;
    bool alert_active_ = false;
    bool network_connected_ = false;
    int network_rssi_ = 0;
    int battery_percent_ = -1;
    bool battery_charging_ = false;
    bool muted_ = false;
    std::string listening_mode_;

    static constexpr size_t kMaxChatLines = 12;

    // Called with mutex_ released whenever something visible changed.
    virtual void OnContentChanged() { Update(); }
};

#endif  // C1XZ_DISPLAY_DISPLAY_H
