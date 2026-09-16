#include "system_info.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/utsname.h>
#include <unistd.h>

#include "platform/log.h"

#define TAG "SystemInfo"

namespace {

std::string ReadFileTrimmed(const char* path, size_t max_len = 256) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return {};
    }
    std::string buffer(max_len, '\0');
    size_t n = fread(&buffer[0], 1, max_len, f);
    fclose(f);
    buffer.resize(n);
    while (!buffer.empty() && (buffer.back() == '\n' || buffer.back() == '\r' ||
                               buffer.back() == ' ' || buffer.back() == '\0')) {
        buffer.pop_back();
    }
    return buffer;
}

size_t ReadMemAvailableBytes() {
    FILE* f = fopen("/proc/meminfo", "r");
    if (f == nullptr) {
        return 0;
    }
    char line[256];
    size_t available = 0;
    size_t free_plus_cached = 0;
    while (fgets(line, sizeof(line), f) != nullptr) {
        unsigned long value = 0;
        if (sscanf(line, "MemAvailable: %lu kB", &value) == 1) {
            available = static_cast<size_t>(value) * 1024;
            break;
        }
        if (sscanf(line, "MemFree: %lu kB", &value) == 1 ||
            sscanf(line, "Cached: %lu kB", &value) == 1) {
            free_plus_cached += static_cast<size_t>(value) * 1024;
        }
    }
    fclose(f);
    // Linux 5.10 always has MemAvailable; the fallback is only for host builds.
    return available != 0 ? available : free_plus_cached;
}

std::mutex g_mutex;
size_t g_min_free = 0;
std::string g_mac;

}  // namespace

size_t SystemInfo::GetFlashSize() {
    // /sys/block/mmcblk0/size counts 512-byte sectors.
    std::string sectors = ReadFileTrimmed("/sys/block/mmcblk0/size");
    if (!sectors.empty()) {
        unsigned long long n = strtoull(sectors.c_str(), nullptr, 10);
        if (n > 0) {
            return static_cast<size_t>(n * 512ULL);
        }
    }
    return 0;
}

size_t SystemInfo::GetFreeHeapSize() {
    size_t available = ReadMemAvailableBytes();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (available != 0 && (g_min_free == 0 || available < g_min_free)) {
        g_min_free = available;
    }
    return available;
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    GetFreeHeapSize();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_min_free;
}

size_t SystemInfo::GetProcessRss() {
    FILE* f = fopen("/proc/self/statm", "r");
    if (f == nullptr) {
        return 0;
    }
    unsigned long total = 0;
    unsigned long resident = 0;
    int matched = fscanf(f, "%lu %lu", &total, &resident);
    fclose(f);
    if (matched != 2) {
        return 0;
    }
    long page = sysconf(_SC_PAGESIZE);
    return static_cast<size_t>(resident) * static_cast<size_t>(page > 0 ? page : 4096);
}

const std::string& SystemInfo::GetMacAddress() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_mac.empty()) {
        return g_mac;
    }
    static const char* kCandidates[] = {
        "/sys/class/net/wlan0/address",
        "/sys/class/net/eth0/address",
        "/sys/class/net/en0/address",
    };
    for (const char* path : kCandidates) {
        std::string value = ReadFileTrimmed(path, 32);
        if (value.size() == 17) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(tolower(c)); });
            g_mac = value;
            return g_mac;
        }
    }
    // Never invent a random address: the server keys the device on it, and a
    // changing Device-Id would silently create a second device on every boot.
    C1XZ_LOGE(TAG, "no MAC address found; the server will reject the handshake");
    g_mac = "00:00:00:00:00:00";
    return g_mac;
}

std::string SystemInfo::GetChipModelName() {
#if C1XZ_DEVICE
    return "x1600";
#else
    return "host";
#endif
}

const char* SystemInfo::GetBoardName() {
    // Board identity affects OTA compatibility on the server side. Do not
    // change this string for a variant; add a new one.
    return "c1-slim-mpd261";
}

std::string SystemInfo::GetUserAgent() {
    return std::string(GetBoardName()) + "/" + C1XZ_VERSION;
}

std::string SystemInfo::GetKernelVersion() {
    utsname info{};
    if (uname(&info) != 0) {
        return {};
    }
    return std::string(info.sysname) + " " + info.release;
}

int64_t SystemInfo::GetUptimeSeconds() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ts.tv_sec;
}

bool SystemInfo::IsClockPlausible() {
    // 2024-01-01. Anything earlier means the RTC lost its time, and TLS chain
    // validation would fail with "certificate not yet valid" rather than
    // anything that points at the real cause.
    return time(nullptr) > 1704067200;
}
