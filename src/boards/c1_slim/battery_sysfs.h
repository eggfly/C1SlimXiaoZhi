#ifndef C1XZ_BOARDS_C1_SLIM_BATTERY_SYSFS_H
#define C1XZ_BOARDS_C1_SLIM_BATTERY_SYSFS_H

namespace c1xz {

// Battery state from /sys/class/power_supply.
//
// The exact supply name on this device has not been confirmed on hardware, so
// this scans the directory rather than hard-coding a path, and reports "no
// battery" instead of guessing when nothing usable is found.
class BatterySysfs {
public:
    struct Snapshot {
        bool present = false;
        int percent = -1;
        bool charging = false;
    };

    static Snapshot Read();
};

}  // namespace c1xz

#endif  // C1XZ_BOARDS_C1_SLIM_BATTERY_SYSFS_H
