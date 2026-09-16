// Host board: the same application, with the hardware replaced by stubs, so
// the protocol and conversation logic can be exercised on a development
// machine. Not a separate code path through the application itself.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "audio/audio_codec.h"
#include "boards/board.h"
#include "display/display.h"
#include "platform/log.h"

#define TAG "HostBoard"

AudioCodec* CreateHostAudioCodec();
Display* CreateHostDisplay();

namespace {

class HostBoard : public Board {
public:
    const char* GetBoardType() const override { return "host"; }

    bool Initialize() override {
        codec_ = CreateHostAudioCodec();
        display_ = CreateHostDisplay();
        if (!codec_->Start() || !display_->Initialize()) {
            return false;
        }
        // Stdin stands in for the keyboard so a conversation can be driven from
        // a terminal or a pipe. One character per line:
        //   t talk   c cancel   q quit   m mode   + louder   - quieter
        running_.store(true);
        input_thread_ = std::thread([this] { ReadKeys(); });
        C1XZ_LOGI(TAG, "keys from stdin: t=talk c=cancel m=mode +/- volume q=quit");
        return true;
    }

    void Shutdown() override {
        running_.store(false);
        if (input_thread_.joinable()) {
            input_thread_.detach();  // blocked on a read from stdin
        }
        if (display_ != nullptr) {
            display_->Shutdown();
        }
        if (codec_ != nullptr) {
            codec_->Stop();
        }
    }

    AudioCodec* GetAudioCodec() override { return codec_; }
    Display* GetDisplay() override { return display_; }

    bool IsNetworkReady() override { return true; }
    std::string GetNetworkName() override { return "host"; }
    std::string GetLocalIp() override { return "127.0.0.1"; }

private:
    AudioCodec* codec_ = nullptr;
    Display* display_ = nullptr;
    std::thread input_thread_;
    std::atomic<bool> running_{false};

    void ReadKeys() {
        char line[64];
        while (running_.load() && fgets(line, sizeof(line), stdin) != nullptr) {
            Key key = Key::kNone;
            switch (line[0]) {
                case 't': key = Key::kTalk; break;
                case 'c': key = Key::kCancel; break;
                case 'q': key = Key::kExit; break;
                case 'm': key = Key::kMode; break;
                case '+': key = Key::kVolumeUp; break;
                case '-': key = Key::kVolumeDown; break;
                default: continue;
            }
            if (key_handler_) {
                // Synthesise a press and release, since a pipe has no edges.
                key_handler_({key, true});
                key_handler_({key, false});
            }
        }
    }
};

}  // namespace

Board& Board::GetInstance() {
    static HostBoard board;
    return board;
}
