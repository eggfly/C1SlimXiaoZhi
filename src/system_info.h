#ifndef C1XZ_SYSTEM_INFO_H
#define C1XZ_SYSTEM_INFO_H

#include <cstddef>
#include <cstdint>
#include <string>

// Same surface as the upstream SystemInfo, backed by procfs and sysfs instead
// of the ESP-IDF heap and efuse APIs.
class SystemInfo {
public:
    // eMMC capacity in bytes, reported where upstream reports flash size.
    static size_t GetFlashSize();

    // System-wide available memory, standing in for the ESP heap figures. The
    // "minimum" value is the low-water mark observed since process start.
    static size_t GetFreeHeapSize();
    static size_t GetMinimumFreeHeapSize();

    // Resident set size of this process, in bytes. Used by the MCP device
    // status tool; upstream has no equivalent because it is not a hosted OS.
    static size_t GetProcessRss();

    // wlan0 hardware address, lower case and colon separated. This is the
    // Device-Id the server identifies the device by, so it must be stable.
    static const std::string& GetMacAddress();

    static std::string GetChipModelName();
    static std::string GetUserAgent();

    // Board identity reported to the server and used as the OTA board name.
    static const char* GetBoardName();

    static std::string GetKernelVersion();
    static int64_t GetUptimeSeconds();

    // True when the wall clock looks plausible. The device has no dedicated RTC
    // battery, so after a long power-off the clock can read 1970, which makes
    // every TLS certificate look "not yet valid".
    static bool IsClockPlausible();
};

#endif  // C1XZ_SYSTEM_INFO_H
