#ifndef C1XZ_BOARDS_BOARD_H
#define C1XZ_BOARDS_BOARD_H

#include <functional>
#include <string>

class Display;
class AudioCodec;

// Hardware abstraction, in the shape of the upstream Board class. Exactly one
// concrete board is linked into a build, and it is reached through
// Board::GetInstance().
class Board {
public:
    virtual ~Board() = default;

    static Board& GetInstance();

    // Stable software identity, persisted on first run. This is the Client-Id
    // header; the server pairs it with the MAC-derived Device-Id.
    const std::string& GetUuid();

    virtual const char* GetBoardType() const = 0;
    virtual bool Initialize() = 0;
    virtual void Shutdown() {}

    virtual AudioCodec* GetAudioCodec() = 0;
    virtual Display* GetDisplay() = 0;

    // Network state, reported in the OTA payload and on the status bar.
    virtual bool IsNetworkReady() = 0;
    virtual std::string GetNetworkName() { return {}; }
    virtual int GetNetworkRssi() { return 0; }
    virtual int GetNetworkChannel() { return 0; }
    virtual std::string GetLocalIp() { return {}; }

    // Returns false when the board cannot report a battery.
    virtual bool GetBatteryLevel(int* percent, bool* charging) {
        (void)percent;
        (void)charging;
        return false;
    }

    // Key events, delivered as one of the logical buttons below.
    enum class Key {
        kNone,
        kTalk,       // Enter / OK: push to talk
        kCancel,     // Back / Esc: abort the response
        kExit,       // Home held: leave the app
        kVolumeUp,
        kVolumeDown,
        kUp,
        kDown,
        kLeft,
        kRight,
        kMode,       // toggles auto/manual listening
    };
    struct KeyEvent {
        Key key = Key::kNone;
        bool pressed = false;  // false means released
    };
    using KeyHandler = std::function<void(const KeyEvent&)>;
    virtual void SetKeyHandler(KeyHandler handler) { key_handler_ = std::move(handler); }

    virtual void Reboot() {}

    // Board-specific object embedded in the OTA request.
    virtual std::string GetBoardJson();

    // Full system information document posted to the OTA endpoint. Mirrors the
    // upstream field set so an unmodified server accepts it.
    std::string GetSystemInfoJson();

protected:
    KeyHandler key_handler_;

private:
    std::string uuid_;
};

#endif  // C1XZ_BOARDS_BOARD_H
