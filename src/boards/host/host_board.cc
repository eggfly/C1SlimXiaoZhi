// Host board: the same application, with the hardware replaced by stubs, so
// the protocol and conversation logic can be exercised on a development
// machine. Not a separate code path through the application itself.

#include <cstdlib>
#include <string>

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
        return codec_->Start() && display_->Initialize();
    }

    void Shutdown() override {
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
};

}  // namespace

Board& Board::GetInstance() {
    static HostBoard board;
    return board;
}
