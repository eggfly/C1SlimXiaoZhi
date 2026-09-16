#ifndef C1XZ_DISPLAY_EPAPER_DISPLAY_H
#define C1XZ_DISPLAY_EPAPER_DISPLAY_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <string>
#include <thread>

#include "display/display.h"
#include "display/font.h"
#include "display/gfx.h"

// 296x152 one-bit e-paper on /dev/epaper_lcd.
//
// The panel needs about 685 ms for a fast refresh and 1690 ms for a full one,
// independent of how much changed. Everything here follows from that:
//
//   - drawing happens on a dedicated thread, never on the caller's
//   - frames are coalesced and rate limited, so callers may mark dirty freely
//   - an identical frame is never submitted
//   - fast refresh is forced on while running, with a full refresh inserted
//     when the whole layout changes, to clear accumulated ghosting
//   - the driver's original refresh settings are restored on exit
class EpaperDisplay : public Display {
public:
    EpaperDisplay();
    ~EpaperDisplay() override;

    bool Initialize() override;
    void Shutdown() override;

    void Update() override;
    void SetPowerSaveMode(bool enable) override;

    int width() const override { return kWidth; }
    int height() const override { return kHeight; }
    bool IsMonochrome() const override { return true; }

    // Requests a ghost-clearing full refresh on the next frame.
    void RequestFullRefresh();

    // Saves the framebuffer the previous owner left, so it can be put back when
    // this app exits. The factory launcher expects to find its desktop intact.
    void SetRestoreFrameOnExit(bool restore) { restore_frame_on_exit_ = restore; }

private:
    static constexpr int kWidth = 296;
    static constexpr int kHeight = 152;
    static constexpr int kFrameBytes = 5624;
    // The panel cannot go faster than this, so submitting more often only
    // queues work and delays the frame the user actually wants to see.
    static constexpr int kMinFrameIntervalMs = 700;

    int fd_ = -1;
    c1xz::Font font_;
    c1xz::Canvas canvas_{kWidth, kHeight};
    c1xz::Canvas last_sent_{kWidth, kHeight};
    bool have_last_sent_ = false;

    std::thread render_thread_;
    std::mutex render_mutex_;
    std::condition_variable render_cv_;
    std::atomic<bool> running_{false};
    bool dirty_ = false;
    bool full_refresh_pending_ = false;
    bool power_save_ = false;
    bool restore_frame_on_exit_ = false;

    std::string saved_fast_refresh_only_;
    std::string saved_refresh_max_;

    void OnContentChanged() override;
    void RenderLoop();
    void Compose();
    void DrawStatusBar();
    void DrawConversation();
    void DrawAlert();
    bool Submit();

    static std::string ReadSysfs(const std::string& attribute);
    static bool WriteSysfs(const std::string& attribute, const std::string& value);
};

#endif  // C1XZ_DISPLAY_EPAPER_DISPLAY_H
