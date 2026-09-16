// Host-build display.
//
// Renders the same 296x152 one-bit canvas the device uses, then prints it as
// ASCII art. That means the layout code under test on a development machine is
// the identical code that runs on the panel, not a separate mock.

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "display/display.h"
#include "display/font.h"
#include "display/gfx.h"
#include "platform/log.h"

#define TAG "Console"

namespace {

class ConsoleDisplay : public Display {
public:
    bool Initialize() override {
        const char* font_path = getenv("C1XZ_FONT");
        if (font_path != nullptr) {
            font_.LoadFile(font_path);
        }
        return true;
    }

    int width() const override { return kWidth; }
    int height() const override { return kHeight; }
    bool IsMonochrome() const override { return true; }

    void Update() override {
        canvas_.Clear(false);
        std::string status;
        std::deque<ChatLine> chat;
        bool alert;
        std::string alert_title;
        std::string alert_message;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status = status_;
            chat = chat_;
            alert = alert_active_;
            alert_title = alert_title_;
            alert_message = alert_message_;
        }

        if (alert) {
            canvas_.DrawRect(0, 0, kWidth, kHeight, true);
            canvas_.DrawText(4, 2, alert_title, font_, true);
            canvas_.DrawTextWrapped(4, font_.height() + 6, kWidth - 8, kHeight - 4, alert_message,
                                    font_, true);
        } else {
            canvas_.DrawText(2, 1, status, font_, true);
            canvas_.DrawHLine(0, font_.height() + 2, kWidth, true);
            int y = font_.height() + 5;
            for (const auto& line : chat) {
                std::string text = (line.role == "user" ? "> " : "") + line.content;
                y = canvas_.DrawTextWrapped(2, y, kWidth - 4, kHeight, text, font_, true);
                if (y >= kHeight) {
                    break;
                }
            }
        }
        Print();
    }

private:
    static constexpr int kWidth = 296;
    static constexpr int kHeight = 152;

    c1xz::Font font_;
    c1xz::Canvas canvas_{kWidth, kHeight};

    void OnContentChanged() override { Update(); }

    void Print() const {
        if (getenv("C1XZ_QUIET_DISPLAY") != nullptr) {
            return;
        }
        printf("+%s+\n", std::string(kWidth / 2, '-').c_str());
        // Two pixels per character horizontally keeps the frame inside a
        // normal terminal.
        for (int y = 0; y < kHeight; y += 4) {
            std::string row;
            row.reserve(kWidth / 2);
            for (int x = 0; x < kWidth; x += 2) {
                bool any = canvas_.GetPixel(x, y) || canvas_.GetPixel(x + 1, y) ||
                           canvas_.GetPixel(x, y + 1) || canvas_.GetPixel(x + 1, y + 1);
                row.push_back(any ? '#' : ' ');
            }
            printf("|%s|\n", row.c_str());
        }
        printf("+%s+\n", std::string(kWidth / 2, '-').c_str());
        fflush(stdout);
    }
};

}  // namespace

Display* CreateHostDisplay() {
    static ConsoleDisplay display;
    return &display;
}
