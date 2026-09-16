#ifndef C1XZ_BOARDS_C1_SLIM_INPUT_EVDEV_H
#define C1XZ_BOARDS_C1_SLIM_INPUT_EVDEV_H

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "boards/board.h"

namespace c1xz {

// Reads the C1 Slim's two input devices and turns raw key codes into the
// logical buttons the application uses.
//
//   /dev/input/event0  matrix keypad (the letter keys)
//   /dev/input/event1  gpio keys (power, home, volume)
//
// event0 is grabbed exclusively so key presses do not leak through to the
// factory launcher underneath; event1 is left shared so the core can still see
// the power key and handle a long press as a shutdown.
class InputEvdev {
public:
    InputEvdev();
    ~InputEvdev();

    InputEvdev(const InputEvdev&) = delete;
    InputEvdev& operator=(const InputEvdev&) = delete;

    void SetHandler(Board::KeyHandler handler) { handler_ = std::move(handler); }

    // Fires after the exit key has been held this long. Matches the launcher
    // convention of a two second Home press to leave an app.
    void SetExitHoldMs(int milliseconds) { exit_hold_ms_ = milliseconds; }

    bool Start();
    void Stop();

    // Blocks until every key is released. Called before exiting so the release
    // event does not land on the launcher and open something.
    void WaitForAllKeysReleased(int timeout_ms = 2000);

    // Overrides the built-in map, as "keycode=name" pairs, e.g. "28=talk".
    // Names: talk, cancel, exit, volume_up, volume_down, up, down, left,
    // right, mode.
    void ApplyKeymapOverride(const std::string& spec);

private:
    struct Device {
        int fd = -1;
        std::string path;
        bool grabbed = false;
    };

    std::vector<Device> devices_;
    Board::KeyHandler handler_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> keys_down_{0};
    int exit_hold_ms_ = 2000;
    std::map<int, Board::Key> keymap_;
    int64_t exit_pressed_at_ms_ = 0;

    void Loop();
    bool OpenDevice(const char* path, bool grab);
    Board::Key Lookup(int code) const;
};

}  // namespace c1xz

#endif  // C1XZ_BOARDS_C1_SLIM_INPUT_EVDEV_H
