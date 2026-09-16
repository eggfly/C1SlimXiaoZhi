#ifndef C1XZ_BOARDS_C1_SLIM_WIFI_STATUS_H
#define C1XZ_BOARDS_C1_SLIM_WIFI_STATUS_H

#include <string>

namespace c1xz {

// Read-only view of wlan0.
//
// This never starts, stops or reconfigures the radio. Wi-Fi on this device is
// owned by the factory scripts or by C1ancher, and both tear the driver module
// down when they turn it off; fighting them over it would be a good way to lose
// the network mid-conversation.
class WifiStatus {
public:
    struct Snapshot {
        bool connected = false;
        std::string ssid;
        std::string ip;
        int rssi = 0;
        int channel = 0;
    };

    // Reads the current state. Cheap enough to poll every few seconds.
    static Snapshot Read();

private:
    static std::string QuerySupplicant(const std::string& command);
};

}  // namespace c1xz

#endif  // C1XZ_BOARDS_C1_SLIM_WIFI_STATUS_H
