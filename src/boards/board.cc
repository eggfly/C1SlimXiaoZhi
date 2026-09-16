#include "boards/board.h"

#include <cJSON.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "display/display.h"
#include "platform/log.h"
#include "settings.h"
#include "system_info.h"

#define TAG "Board"

namespace {

std::string GenerateUuidV4() {
    unsigned char bytes[16];
    FILE* f = fopen("/dev/urandom", "rb");
    if (f != nullptr && fread(bytes, 1, sizeof(bytes), f) == sizeof(bytes)) {
        fclose(f);
    } else {
        if (f != nullptr) {
            fclose(f);
        }
        // Never silently fall back to a weak identifier: a colliding Client-Id
        // would attach this device to someone else's session.
        C1XZ_LOGE(TAG, "cannot read /dev/urandom to generate a client id");
        return {};
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);  // variant 1

    char text[37];
    snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return text;
}

}  // namespace

const std::string& Board::GetUuid() {
    if (!uuid_.empty()) {
        return uuid_;
    }
    Settings settings("board", true);
    uuid_ = settings.GetString("uuid");
    if (uuid_.empty()) {
        uuid_ = GenerateUuidV4();
        if (!uuid_.empty()) {
            settings.SetString("uuid", uuid_);
            C1XZ_LOGI(TAG, "generated a new client id: %s", uuid_.c_str());
        }
    }
    return uuid_;
}

std::string Board::GetBoardJson() {
    cJSON* board = cJSON_CreateObject();
    cJSON_AddStringToObject(board, "type", GetBoardType());
    cJSON_AddStringToObject(board, "name", SystemInfo::GetBoardName());
    std::string ssid = GetNetworkName();
    if (!ssid.empty()) {
        cJSON_AddStringToObject(board, "ssid", ssid.c_str());
        cJSON_AddNumberToObject(board, "rssi", GetNetworkRssi());
        cJSON_AddNumberToObject(board, "channel", GetNetworkChannel());
    }
    std::string ip = GetLocalIp();
    if (!ip.empty()) {
        cJSON_AddStringToObject(board, "ip", ip.c_str());
    }
    cJSON_AddStringToObject(board, "mac", SystemInfo::GetMacAddress().c_str());

    char* text = cJSON_PrintUnformatted(board);
    std::string json(text != nullptr ? text : "{}");
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(board);
    return json;
}

std::string Board::GetSystemInfoJson() {
    // Field set mirrors upstream Board::GetSystemInfoJson so that an unmodified
    // xiaozhi server accepts the OTA request. Fields that only make sense on an
    // ESP32 (partition table, chip revision, elf sha) are reported with values
    // that describe this device honestly instead of being faked.
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", "zh-CN");
    cJSON_AddNumberToObject(root, "flash_size", static_cast<double>(SystemInfo::GetFlashSize()));
    cJSON_AddStringToObject(
        root, "minimum_free_heap_size",
        std::to_string(SystemInfo::GetMinimumFreeHeapSize()).c_str());
    cJSON_AddStringToObject(root, "mac_address", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "uuid", GetUuid().c_str());
    cJSON_AddStringToObject(root, "chip_model_name", SystemInfo::GetChipModelName().c_str());

    cJSON* chip_info = cJSON_CreateObject();
    cJSON_AddNumberToObject(chip_info, "model", 0);
    cJSON_AddNumberToObject(chip_info, "cores", 1);
    cJSON_AddNumberToObject(chip_info, "revision", 0);
    cJSON_AddNumberToObject(chip_info, "features", 0);
    cJSON_AddItemToObject(root, "chip_info", chip_info);

    cJSON* application = cJSON_CreateObject();
    cJSON_AddStringToObject(application, "name", "c1xiaozhi");
    cJSON_AddStringToObject(application, "version", C1XZ_VERSION);
    // Injected by CMake rather than __DATE__/__TIME__, which would make the
    // build unreproducible. Empty unless -DC1XZ_BUILD_TIME was given.
    cJSON_AddStringToObject(application, "compile_time", C1XZ_BUILD_TIME);
    cJSON_AddStringToObject(application, "idf_version", SystemInfo::GetKernelVersion().c_str());
    cJSON_AddStringToObject(application, "elf_sha256", "");
    cJSON_AddItemToObject(root, "application", application);

    cJSON* partitions = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "partition_table", partitions);
    cJSON* ota = cJSON_CreateObject();
    cJSON_AddStringToObject(ota, "label", "storage");
    cJSON_AddItemToObject(root, "ota", ota);

    Display* display = GetDisplay();
    if (display != nullptr) {
        cJSON* display_info = cJSON_CreateObject();
        cJSON_AddBoolToObject(display_info, "monochrome", display->IsMonochrome());
        cJSON_AddNumberToObject(display_info, "width", display->width());
        cJSON_AddNumberToObject(display_info, "height", display->height());
        cJSON_AddItemToObject(root, "display", display_info);
    }

    cJSON* board = cJSON_Parse(GetBoardJson().c_str());
    if (board != nullptr) {
        cJSON_AddItemToObject(root, "board", board);
    }

    char* text = cJSON_PrintUnformatted(root);
    std::string json(text != nullptr ? text : "{}");
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return json;
}
