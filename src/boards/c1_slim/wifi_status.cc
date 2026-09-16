#include "boards/c1_slim/wifi_status.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "platform/log.h"

#define TAG "Wifi"

namespace c1xz {
namespace {

const char kInterface[] = "wlan0";
const char kSupplicantDir[] = "/var/run/wpa_supplicant";

std::string ReadLine(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        return {};
    }
    char buffer[128] = {0};
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

std::string GetIpv4() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return {};
    }
    ifreq request{};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", kInterface);
    request.ifr_addr.sa_family = AF_INET;
    std::string address;
    if (ioctl(fd, SIOCGIFADDR, &request) == 0) {
        char text[INET_ADDRSTRLEN] = {0};
        sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(&request.ifr_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) != nullptr) {
            address = text;
        }
    }
    close(fd);
    return address;
}

int FrequencyToChannel(int mhz) {
    if (mhz >= 2412 && mhz <= 2472) {
        return (mhz - 2412) / 5 + 1;
    }
    if (mhz == 2484) {
        return 14;
    }
    if (mhz >= 5180 && mhz <= 5825) {
        return (mhz - 5000) / 5;
    }
    return 0;
}

std::string FieldFromStatus(const std::string& status, const std::string& key) {
    size_t pos = 0;
    while (pos < status.size()) {
        size_t end = status.find('\n', pos);
        if (end == std::string::npos) {
            end = status.size();
        }
        std::string line = status.substr(pos, end - pos);
        pos = end + 1;
        size_t equals = line.find('=');
        if (equals != std::string::npos && line.compare(0, equals, key) == 0) {
            return line.substr(equals + 1);
        }
    }
    return {};
}

}  // namespace

std::string WifiStatus::QuerySupplicant(const std::string& command) {
    std::string remote_path = std::string(kSupplicantDir) + "/" + kInterface;
    sockaddr_un remote{};
    remote.sun_family = AF_UNIX;
    if (remote_path.size() + 1 > sizeof(remote.sun_path)) {
        return {};
    }
    snprintf(remote.sun_path, sizeof(remote.sun_path), "%s", remote_path.c_str());

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return {};
    }

    // wpa_supplicant replies to the address we bind, so we need our own
    // abstract-namespace socket to receive on.
    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/c1xz-wpa-%d", getpid());
    unlink(local.sun_path);
    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        close(fd);
        return {};
    }

    std::string reply;
    if (connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0 &&
        send(fd, command.data(), command.size(), 0) == static_cast<ssize_t>(command.size())) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 500) > 0) {
            char buffer[4096];
            ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
            if (n > 0) {
                buffer[n] = '\0';
                reply = buffer;
            }
        }
    }

    close(fd);
    unlink(local.sun_path);
    return reply;
}

WifiStatus::Snapshot WifiStatus::Read() {
    Snapshot snapshot;

    std::string carrier = ReadLine("/sys/class/net/wlan0/carrier");
    snapshot.ip = GetIpv4();
    snapshot.connected = carrier == "1" && !snapshot.ip.empty();

    std::string status = QuerySupplicant("STATUS");
    if (!status.empty()) {
        snapshot.ssid = FieldFromStatus(status, "ssid");
        std::string state = FieldFromStatus(status, "wpa_state");
        if (state != "COMPLETED") {
            snapshot.connected = false;
        }
        std::string freq = FieldFromStatus(status, "freq");
        if (!freq.empty()) {
            snapshot.channel = FrequencyToChannel(atoi(freq.c_str()));
        }
    }

    // /proc/net/wireless reports link quality per interface; column 4 is the
    // signal level in dBm.
    FILE* f = fopen("/proc/net/wireless", "r");
    if (f != nullptr) {
        char line[256];
        while (fgets(line, sizeof(line), f) != nullptr) {
            if (strstr(line, kInterface) == nullptr) {
                continue;
            }
            char name[32];
            int status_field;
            float link, level, noise;
            if (sscanf(line, " %31[^:]: %d %f %f %f", name, &status_field, &link, &level,
                       &noise) == 5) {
                snapshot.rssi = static_cast<int>(level);
            }
            break;
        }
        fclose(f);
    }

    return snapshot;
}

}  // namespace c1xz
