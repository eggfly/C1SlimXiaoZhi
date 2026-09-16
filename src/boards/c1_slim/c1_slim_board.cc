// The one board this build targets: 快易典 C1 Slim / MP-D261.
//
// Ingenic X1600E, MIPS32r2 at 1 GHz, ~50 MiB RAM, Linux 5.10.186.
// ES8326 audio codec on ALSA card 0, 296x152 one-bit e-paper, matrix keyboard.

#include <memory>

#include "audio/tinyalsa_audio_codec.h"
#include "platform/event_loop.h"
#include "boards/board.h"
#include "boards/c1_slim/battery_sysfs.h"
#include "boards/c1_slim/input_evdev.h"
#include "boards/c1_slim/launcher_lease.h"
#include "boards/c1_slim/wifi_status.h"
#include "display/epaper_display.h"
#include "platform/log.h"

#define TAG "C1Slim"

namespace {

class C1SlimBoard : public Board {
public:
    const char* GetBoardType() const override { return "c1-slim"; }

    bool Initialize() override {
        if (!lease_.Acquire()) {
            return false;
        }
        // Pause the factory UI rather than killing it: its supervisor restarts
        // it about a second after a kill, and a paused process keeps its
        // memory so the way back is instant.
        lease_.SuspendForeignUi();

        codec_.reset(new TinyAlsaAudioCodec());
        if (!codec_->Start()) {
            C1XZ_LOGE(TAG, "audio codec failed to start");
            return false;
        }

        display_.reset(new EpaperDisplay());
        if (!display_->Initialize()) {
            C1XZ_LOGE(TAG, "display failed to start");
            return false;
        }

        input_.reset(new c1xz::InputEvdev());
        input_->SetHandler([this](const KeyEvent& event) {
            if (key_handler_) {
                key_handler_(event);
            }
        });
        if (!input_->Start()) {
            C1XZ_LOGE(TAG, "input failed to start");
            return false;
        }
        return true;
    }

    void Shutdown() override {
        if (input_ != nullptr) {
            // Swallow the release of whatever key triggered the exit, so it
            // does not land on the launcher and open something.
            input_->WaitForAllKeysReleased();
            input_->Stop();
        }
        if (display_ != nullptr) {
            display_->Shutdown();
        }
        if (codec_ != nullptr) {
            codec_->Stop();
        }
        lease_.Release();
    }

    AudioCodec* GetAudioCodec() override { return codec_.get(); }
    Display* GetDisplay() override { return display_.get(); }

    bool IsNetworkReady() override { return Refresh().connected; }
    std::string GetNetworkName() override { return Refresh().ssid; }
    int GetNetworkRssi() override { return Refresh().rssi; }
    int GetNetworkChannel() override { return Refresh().channel; }
    std::string GetLocalIp() override { return Refresh().ip; }

    bool GetBatteryLevel(int* percent, bool* charging) override {
        auto snapshot = c1xz::BatterySysfs::Read();
        if (!snapshot.present) {
            return false;
        }
        if (percent != nullptr) {
            *percent = snapshot.percent;
        }
        if (charging != nullptr) {
            *charging = snapshot.charging;
        }
        return true;
    }

    void Reboot() override {
        // Deliberately not implemented: this application runs as root on a
        // device with a read-only rootfs and no recovery path we control.
        // Rebooting on a server command would be an easy way to brick a
        // session in the middle of an OTA write.
        C1XZ_LOGW(TAG, "ignoring a reboot request; not supported on this board");
    }

private:
    std::unique_ptr<TinyAlsaAudioCodec> codec_;
    std::unique_ptr<EpaperDisplay> display_;
    std::unique_ptr<c1xz::InputEvdev> input_;
    c1xz::LauncherLease lease_;

    c1xz::WifiStatus::Snapshot cached_;
    int64_t cached_at_ms_ = 0;

    // Querying wpa_supplicant costs a round trip on a unix socket; the status
    // bar asks several times per redraw, so cache briefly.
    const c1xz::WifiStatus::Snapshot& Refresh() {
        int64_t now = c1xz::MonotonicMs();
        if (cached_at_ms_ == 0 || now - cached_at_ms_ > 3000) {
            cached_ = c1xz::WifiStatus::Read();
            cached_at_ms_ = now;
        }
        return cached_;
    }
};

}  // namespace

Board& Board::GetInstance() {
    static C1SlimBoard board;
    return board;
}
