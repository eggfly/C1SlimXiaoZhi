#include "display/display.h"

#include <cstdlib>

#include "platform/event_loop.h"

void Display::SetStatus(const std::string& status) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ == status) {
            return;
        }
        status_ = status;
    }
    OnContentChanged();
}

void Display::ShowNotification(const std::string& notification, int duration_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notification_ = notification;
        notification_expires_ms_ = c1xz::MonotonicMs() + duration_ms;
    }
    OnContentChanged();
}

void Display::SetEmotion(const std::string& emotion) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (emotion_ == emotion) {
            return;
        }
        emotion_ = emotion;
    }
    OnContentChanged();
}

void Display::SetChatMessage(const std::string& role, const std::string& content) {
    if (content.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The server streams a response sentence by sentence, each one a fresh
        // SetChatMessage with the same role. Replace the previous line from the
        // same speaker instead of stacking partial sentences.
        if (!chat_.empty() && chat_.back().role == role) {
            chat_.back().content = content;
        } else {
            chat_.push_back({role, content});
        }
        while (chat_.size() > kMaxChatLines) {
            chat_.pop_front();
        }
    }
    OnContentChanged();
}

void Display::ClearChatMessages() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chat_.clear();
    }
    OnContentChanged();
}

void Display::ShowAlert(const std::string& title, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        alert_title_ = title;
        alert_message_ = message;
        alert_active_ = true;
    }
    OnContentChanged();
}

void Display::DismissAlert() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!alert_active_) {
            return;
        }
        alert_active_ = false;
        alert_title_.clear();
        alert_message_.clear();
    }
    OnContentChanged();
}

void Display::SetNetworkState(bool connected, int rssi) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (network_connected_ == connected && network_rssi_ == rssi) {
            return;
        }
        network_connected_ = connected;
        network_rssi_ = rssi;
    }
    OnContentChanged();
}

void Display::SetBatteryState(int percent, bool charging) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Ignore one-percent jitter: every redraw costs 685 ms on e-paper.
        if (battery_charging_ == charging && battery_percent_ >= 0 &&
            std::abs(battery_percent_ - percent) < 5) {
            return;
        }
        battery_percent_ = percent;
        battery_charging_ = charging;
    }
    OnContentChanged();
}

void Display::SetMuted(bool muted) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (muted_ == muted) {
            return;
        }
        muted_ = muted;
    }
    OnContentChanged();
}

void Display::SetListeningMode(const std::string& mode) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (listening_mode_ == mode) {
            return;
        }
        listening_mode_ = mode;
    }
    OnContentChanged();
}
