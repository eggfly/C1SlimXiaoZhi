#include "boards/c1_slim/input_evdev.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "platform/event_loop.h"
#include "platform/log.h"

#define TAG "Input"

namespace c1xz {
namespace {

// _IOW('E', 0x90, int) encodes differently on MIPS than on x86, so the value is
// spelled out rather than built with the macro.
constexpr unsigned long kEviocGrabMips = 0x80044590UL;

// Linux key codes. The exact matrix layout of this keyboard has not been
// captured yet, so the defaults cover the keys whose codes are standard, and
// anything unrecognised is logged with its code so it can be mapped with
// $C1XZ_KEYMAP without rebuilding.
const struct {
    int code;
    Board::Key key;
} kDefaultKeymap[] = {
    {KEY_ENTER, Board::Key::kTalk},
    {KEY_KPENTER, Board::Key::kTalk},
    {KEY_SPACE, Board::Key::kTalk},
    {KEY_ESC, Board::Key::kCancel},
    {KEY_BACK, Board::Key::kCancel},
    {KEY_BACKSPACE, Board::Key::kCancel},
    {KEY_HOME, Board::Key::kExit},
    {KEY_VOLUMEUP, Board::Key::kVolumeUp},
    {KEY_VOLUMEDOWN, Board::Key::kVolumeDown},
    {KEY_UP, Board::Key::kUp},
    {KEY_DOWN, Board::Key::kDown},
    {KEY_LEFT, Board::Key::kLeft},
    {KEY_RIGHT, Board::Key::kRight},
    {KEY_M, Board::Key::kMode},
};

Board::Key KeyFromName(const std::string& name) {
    if (name == "talk") return Board::Key::kTalk;
    if (name == "cancel") return Board::Key::kCancel;
    if (name == "exit") return Board::Key::kExit;
    if (name == "volume_up") return Board::Key::kVolumeUp;
    if (name == "volume_down") return Board::Key::kVolumeDown;
    if (name == "up") return Board::Key::kUp;
    if (name == "down") return Board::Key::kDown;
    if (name == "left") return Board::Key::kLeft;
    if (name == "right") return Board::Key::kRight;
    if (name == "mode") return Board::Key::kMode;
    return Board::Key::kNone;
}

}  // namespace

InputEvdev::InputEvdev() {
    for (const auto& entry : kDefaultKeymap) {
        keymap_[entry.code] = entry.key;
    }
    const char* override_spec = getenv("C1XZ_KEYMAP");
    if (override_spec != nullptr) {
        ApplyKeymapOverride(override_spec);
    }
}

InputEvdev::~InputEvdev() { Stop(); }

void InputEvdev::ApplyKeymapOverride(const std::string& spec) {
    size_t start = 0;
    while (start < spec.size()) {
        size_t end = spec.find_first_of(",; ", start);
        if (end == std::string::npos) {
            end = spec.size();
        }
        std::string pair = spec.substr(start, end - start);
        start = end + 1;
        size_t equals = pair.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        int code = atoi(pair.substr(0, equals).c_str());
        Board::Key key = KeyFromName(pair.substr(equals + 1));
        if (code > 0 && key != Board::Key::kNone) {
            keymap_[code] = key;
            C1XZ_LOGI(TAG, "keymap override: code %d -> %s", code,
                      pair.substr(equals + 1).c_str());
        }
    }
}

Board::Key InputEvdev::Lookup(int code) const {
    auto it = keymap_.find(code);
    return it == keymap_.end() ? Board::Key::kNone : it->second;
}

bool InputEvdev::OpenDevice(const char* path, bool grab) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        C1XZ_LOGW(TAG, "cannot open %s: %s", path, strerror(errno));
        return false;
    }
    Device device;
    device.fd = fd;
    device.path = path;
    if (grab) {
        int one = 1;
        if (ioctl(fd, kEviocGrabMips, &one) == 0) {
            device.grabbed = true;
        } else {
            // Not fatal: without the grab, key presses also reach whatever is
            // behind us, which is untidy but still usable.
            C1XZ_LOGW(TAG, "could not grab %s: %s", path, strerror(errno));
        }
    }
    devices_.push_back(device);
    C1XZ_LOGI(TAG, "opened %s%s", path, device.grabbed ? " (exclusive)" : "");
    return true;
}

bool InputEvdev::Start() {
    // The letter matrix is grabbed so typing does not reach the launcher. The
    // gpio keys are left shared so the system still handles a long power press.
    OpenDevice("/dev/input/event0", true);
    OpenDevice("/dev/input/event1", false);
    if (devices_.empty()) {
        C1XZ_LOGE(TAG, "no input devices available");
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void InputEvdev::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    for (auto& device : devices_) {
        if (device.grabbed) {
            int zero = 0;
            ioctl(device.fd, kEviocGrabMips, &zero);
        }
        if (device.fd >= 0) {
            close(device.fd);
        }
    }
    devices_.clear();
}

void InputEvdev::Loop() {
    std::vector<pollfd> fds;
    fds.reserve(devices_.size());
    for (const auto& device : devices_) {
        pollfd pfd{};
        pfd.fd = device.fd;
        pfd.events = POLLIN;
        fds.push_back(pfd);
    }

    while (running_.load()) {
        int rc = poll(fds.data(), fds.size(), 200);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        // A held exit key fires once the hold time is reached, without waiting
        // for the release, so the user gets feedback while still holding it.
        if (exit_pressed_at_ms_ != 0 &&
            MonotonicMs() - exit_pressed_at_ms_ >= exit_hold_ms_) {
            exit_pressed_at_ms_ = 0;
            if (handler_) {
                handler_({Board::Key::kExit, true});
            }
        }
        if (rc == 0) {
            continue;
        }

        for (size_t i = 0; i < fds.size(); ++i) {
            if ((fds[i].revents & POLLIN) == 0) {
                continue;
            }
            input_event events[32];
            ssize_t bytes = read(fds[i].fd, events, sizeof(events));
            if (bytes < static_cast<ssize_t>(sizeof(input_event))) {
                continue;
            }
            size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
            for (size_t k = 0; k < count; ++k) {
                if (events[k].type != EV_KEY) {
                    continue;
                }
                // value 2 is auto-repeat; the application wants edges only.
                if (events[k].value == 2) {
                    continue;
                }
                bool pressed = events[k].value == 1;
                keys_down_.fetch_add(pressed ? 1 : -1);
                if (keys_down_.load() < 0) {
                    keys_down_.store(0);
                }

                Board::Key key = Lookup(events[k].code);
                if (key == Board::Key::kNone) {
                    C1XZ_LOGD(TAG, "unmapped key code %d (%s)", events[k].code,
                              pressed ? "down" : "up");
                    continue;
                }

                if (key == Board::Key::kExit) {
                    // Exit is a hold, not a tap, so a stray press does not drop
                    // the user out of a conversation.
                    if (pressed) {
                        exit_pressed_at_ms_ = MonotonicMs();
                    } else {
                        exit_pressed_at_ms_ = 0;
                    }
                    continue;
                }

                if (handler_) {
                    handler_({key, pressed});
                }
            }
        }
    }
}

void InputEvdev::WaitForAllKeysReleased(int timeout_ms) {
    int64_t deadline = MonotonicMs() + timeout_ms;
    while (keys_down_.load() > 0 && MonotonicMs() < deadline) {
        usleep(20 * 1000);
    }
}

}  // namespace c1xz
