#include "boards/c1_slim/battery_sysfs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>

#include "platform/log.h"

#define TAG "Battery"

namespace c1xz {
namespace {

const char kSupplyRoot[] = "/sys/class/power_supply";

std::string ReadAttribute(const std::string& directory, const char* name) {
    std::string path = directory + "/" + name;
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

}  // namespace

BatterySysfs::Snapshot BatterySysfs::Read() {
    Snapshot snapshot;

    DIR* dir = opendir(kSupplyRoot);
    if (dir == nullptr) {
        return snapshot;
    }

    dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        std::string directory = std::string(kSupplyRoot) + "/" + entry->d_name;
        std::string type = ReadAttribute(directory, "type");
        if (type != "Battery") {
            continue;
        }

        std::string capacity = ReadAttribute(directory, "capacity");
        if (capacity.empty()) {
            continue;
        }
        snapshot.present = true;
        snapshot.percent = atoi(capacity.c_str());
        if (snapshot.percent < 0) {
            snapshot.percent = 0;
        }
        if (snapshot.percent > 100) {
            snapshot.percent = 100;
        }

        std::string status = ReadAttribute(directory, "status");
        snapshot.charging = status == "Charging" || status == "Full";
        break;
    }
    closedir(dir);

    return snapshot;
}

}  // namespace c1xz
