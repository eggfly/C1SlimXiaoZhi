#include "display/epaper_display.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <deque>
#include <vector>

#include "platform/event_loop.h"
#include "platform/log.h"

#define TAG "Epaper"

namespace {

const char kDevice[] = "/dev/epaper_lcd";
const char kSysfsDir[] = "/sys/devices/platform/e0266a128/epaper/";

std::string EnvOr(const char* name, const char* fallback) {
    const char* value = getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

}  // namespace

EpaperDisplay::EpaperDisplay() = default;

EpaperDisplay::~EpaperDisplay() { Shutdown(); }

std::string EpaperDisplay::ReadSysfs(const std::string& attribute) {
    std::string path = std::string(kSysfsDir) + attribute;
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr) {
        return {};
    }
    char buffer[64] = {0};
    if (fgets(buffer, sizeof(buffer), f) == nullptr) {
        fclose(f);
        return {};
    }
    fclose(f);
    std::string value(buffer);
    while (!value.empty() && (value.back() == '\n' || value.back() == ' ')) {
        value.pop_back();
    }
    return value;
}

bool EpaperDisplay::WriteSysfs(const std::string& attribute, const std::string& value) {
    std::string path = std::string(kSysfsDir) + attribute;
    FILE* f = fopen(path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    bool ok = fputs(value.c_str(), f) >= 0;
    fclose(f);
    return ok;
}

bool EpaperDisplay::Initialize() {
    std::string font_path = EnvOr("C1XZ_FONT", "/storage/c1/xiaozhi/font.bin");
    font_.LoadFile(font_path);

    fd_ = open(kDevice, O_WRONLY | O_CLOEXEC);
    if (fd_ < 0) {
        C1XZ_LOGE(TAG, "cannot open %s: %s", kDevice, strerror(errno));
        return false;
    }

    // Remember the driver's settings so they can be restored; the factory UI
    // and other apps rely on them.
    saved_fast_refresh_only_ = ReadSysfs("fast_refresh_only");
    saved_refresh_max_ = ReadSysfs("refresh_max");

    // Suppress the driver's periodic automatic full refresh. We decide when a
    // full refresh is worth its 1690 ms.
    WriteSysfs("fast_refresh_only", "1");

    running_.store(true);
    render_thread_ = std::thread([this] { RenderLoop(); });
    RequestFullRefresh();
    C1XZ_LOGI(TAG, "display ready (%dx%d, 1bpp)", kWidth, kHeight);
    return true;
}

void EpaperDisplay::Shutdown() {
    if (!running_.exchange(false)) {
        return;
    }
    render_cv_.notify_all();
    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    if (!saved_fast_refresh_only_.empty()) {
        WriteSysfs("fast_refresh_only", saved_fast_refresh_only_);
    }
    if (!saved_refresh_max_.empty()) {
        WriteSysfs("refresh_max", saved_refresh_max_);
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void EpaperDisplay::OnContentChanged() {
    {
        std::lock_guard<std::mutex> lock(render_mutex_);
        dirty_ = true;
    }
    render_cv_.notify_all();
}

void EpaperDisplay::Update() { OnContentChanged(); }

void EpaperDisplay::RequestFullRefresh() {
    {
        std::lock_guard<std::mutex> lock(render_mutex_);
        full_refresh_pending_ = true;
        dirty_ = true;
    }
    render_cv_.notify_all();
}

void EpaperDisplay::SetPowerSaveMode(bool enable) {
    {
        std::lock_guard<std::mutex> lock(render_mutex_);
        power_save_ = enable;
    }
    OnContentChanged();
}

void EpaperDisplay::RenderLoop() {
    int64_t last_frame_ms = 0;
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(render_mutex_);
            render_cv_.wait_for(lock, std::chrono::milliseconds(200),
                                [this] { return dirty_ || !running_.load(); });
            if (!running_.load()) {
                break;
            }
            if (!dirty_) {
                continue;
            }
            // Coalesce: wait out the panel's minimum interval, letting any
            // further changes in that window fold into the same frame.
            int64_t now = c1xz::MonotonicMs();
            int64_t wait = kMinFrameIntervalMs - (now - last_frame_ms);
            if (wait > 0) {
                render_cv_.wait_for(lock, std::chrono::milliseconds(wait));
                if (!running_.load()) {
                    break;
                }
            }
            dirty_ = false;
        }

        Compose();
        if (Submit()) {
            last_frame_ms = c1xz::MonotonicMs();
        }
    }
}

void EpaperDisplay::Compose() {
    canvas_.Clear(false);
    bool alert;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        alert = alert_active_;
    }
    if (alert) {
        DrawAlert();
        return;
    }
    DrawStatusBar();
    DrawConversation();
}

void EpaperDisplay::DrawStatusBar() {
    std::string status;
    std::string notification;
    std::string mode;
    bool network;
    int rssi;
    int battery;
    bool charging;
    bool muted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notification =
            c1xz::MonotonicMs() < notification_expires_ms_ ? notification_ : std::string();
        status = status_;
        mode = listening_mode_;
        network = network_connected_;
        rssi = network_rssi_;
        battery = battery_percent_;
        charging = battery_charging_;
        muted = muted_;
    }

    const int bar_height = font_.height() + 2;
    std::string left = notification.empty() ? status : notification;
    if (!notification.empty()) {
        // A notification takes the whole bar and is shown inverted so it reads
        // as transient rather than as the steady state.
        canvas_.FillRect(0, 0, kWidth, bar_height, true);
        canvas_.DrawText(2, 1, left, font_, false);
    } else {
        canvas_.DrawText(2, 1, left, font_, true);

        // Right side indicators, laid out from the right edge inward.
        int pen = kWidth - 2;
        if (battery >= 0) {
            char text[16];
            snprintf(text, sizeof(text), "%d%%%s", battery, charging ? "+" : "");
            int w = font_.MeasureUtf8(text);
            pen -= w;
            canvas_.DrawText(pen, 1, text, font_, true);
            pen -= 6;
        }
        const char* net = network ? "WiFi" : "----";
        int net_width = font_.MeasureUtf8(net);
        pen -= net_width;
        canvas_.DrawText(pen, 1, net, font_, true);
        (void)rssi;

        if (muted) {
            const char* tag = "MUTE";
            pen -= font_.MeasureUtf8(tag) + 6;
            canvas_.DrawText(pen, 1, tag, font_, true);
        }
        if (!mode.empty()) {
            pen -= font_.MeasureUtf8(mode) + 6;
            canvas_.DrawText(pen, 1, mode, font_, true);
        }
    }
    canvas_.DrawHLine(0, bar_height, kWidth, true);
}

void EpaperDisplay::DrawConversation() {
    std::deque<ChatLine> chat;
    std::string emotion;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chat = chat_;
        emotion = emotion_;
    }

    const int top = font_.height() + 5;
    const int bottom = kHeight;
    const int line_height = font_.height();

    if (chat.empty()) {
        std::string hint = emotion.empty() ? "按 OK 开始对话" : emotion;
        int text_width = font_.MeasureUtf8(hint);
        canvas_.DrawText((kWidth - text_width) / 2, (top + bottom) / 2 - line_height / 2, hint,
                         font_, true);
        return;
    }

    // Lay the conversation out from the bottom up so the newest text is always
    // on screen, then draw only the lines that fit.
    struct Block {
        std::string prefix;
        std::string content;
        int height;
    };
    std::vector<Block> blocks;
    int total = 0;
    for (auto it = chat.rbegin(); it != chat.rend(); ++it) {
        Block block;
        block.prefix = it->role == "user" ? "> " : "";
        block.content = block.prefix + it->content;
        block.height = canvas_.MeasureWrappedHeight(kWidth - 4, block.content, font_);
        if (total + block.height > bottom - top) {
            break;
        }
        total += block.height;
        blocks.push_back(std::move(block));
    }

    int y = bottom - total;
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        y = canvas_.DrawTextWrapped(2, y, kWidth - 4, bottom, it->content, font_, true);
    }
}

void EpaperDisplay::DrawAlert() {
    std::string title;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        title = alert_title_;
        message = alert_message_;
    }

    canvas_.DrawRect(0, 0, kWidth, kHeight, true);
    const int line_height = font_.height();
    int title_width = font_.MeasureUtf8(title);
    canvas_.FillRect(1, 1, kWidth - 2, line_height + 2, true);
    canvas_.DrawText((kWidth - title_width) / 2, 2, title, font_, false);

    int body_height = canvas_.MeasureWrappedHeight(kWidth - 16, message, font_);
    int y = (kHeight + line_height) / 2 - body_height / 2;
    if (y < line_height + 8) {
        y = line_height + 8;
    }
    canvas_.DrawTextWrapped(8, y, kWidth - 16, kHeight - 4, message, font_, true);
}

bool EpaperDisplay::Submit() {
    if (fd_ < 0) {
        return false;
    }
    bool full;
    {
        std::lock_guard<std::mutex> lock(render_mutex_);
        full = full_refresh_pending_;
        full_refresh_pending_ = false;
    }

    // The driver drops a frame identical to the previous one, and so do we, to
    // avoid even the syscall and the 13-22 ms it spends noticing.
    if (have_last_sent_ && canvas_ == last_sent_ && !full) {
        return false;
    }

    if (full) {
        // refresh=1 does not refresh immediately: it makes the next write with
        // different content a full refresh. So arm it, then write.
        WriteSysfs("fast_refresh_only", "0");
        WriteSysfs("refresh", "1");
    }

    ssize_t written = write(fd_, canvas_.data(), canvas_.size());
    if (written != static_cast<ssize_t>(canvas_.size())) {
        C1XZ_LOGE(TAG, "short write to %s: %zd of %zu", kDevice, written, canvas_.size());
        return false;
    }

    if (full) {
        // A full refresh takes about 1690 ms; wait it out before restoring the
        // fast-only setting, otherwise the driver may coalesce the next frame
        // into the slow one.
        usleep(1800 * 1000);
        WriteSysfs("fast_refresh_only", "1");
    }

    last_sent_ = canvas_;
    have_last_sent_ = true;
    return true;
}
