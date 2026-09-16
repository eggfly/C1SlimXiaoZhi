#include "settings.h"

#include <cJSON.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/log.h"

#define TAG "Settings"

namespace {

std::mutex g_mutex;
cJSON* g_root = nullptr;
bool g_loaded = false;
bool g_dirty = false;
std::string g_path;

std::string DefaultPath() {
    const char* env = getenv("C1XZ_SETTINGS");
    if (env != nullptr && *env != '\0') {
        return env;
    }
#if C1XZ_DEVICE
    return "/storage/c1/xiaozhi/settings.json";
#else
    const char* home = getenv("HOME");
    return std::string(home != nullptr ? home : ".") + "/.c1xiaozhi/settings.json";
#endif
}

void MakeParentDirs(const std::string& path) {
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        mkdir(path.substr(0, pos).c_str(), 0755);
    }
}

// Caller holds g_mutex.
void LoadLocked() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;
    if (g_path.empty()) {
        g_path = DefaultPath();
    }
    g_root = nullptr;

    FILE* f = fopen(g_path.c_str(), "rb");
    if (f != nullptr) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0 && size < 4 * 1024 * 1024) {
            std::string buffer(static_cast<size_t>(size), '\0');
            if (fread(&buffer[0], 1, static_cast<size_t>(size), f) == static_cast<size_t>(size)) {
                g_root = cJSON_Parse(buffer.c_str());
                if (g_root == nullptr) {
                    C1XZ_LOGW(TAG, "settings file is not valid JSON, starting empty: %s",
                              g_path.c_str());
                }
            }
        }
        fclose(f);
    }
    if (g_root == nullptr || !cJSON_IsObject(g_root)) {
        if (g_root != nullptr) {
            cJSON_Delete(g_root);
        }
        g_root = cJSON_CreateObject();
    }
}

// Caller holds g_mutex.
cJSON* NamespaceLocked(const std::string& ns, bool create) {
    LoadLocked();
    cJSON* item = cJSON_GetObjectItem(g_root, ns.c_str());
    if (item != nullptr && cJSON_IsObject(item)) {
        return item;
    }
    if (item != nullptr) {
        cJSON_DeleteItemFromObject(g_root, ns.c_str());
    }
    if (!create) {
        return nullptr;
    }
    cJSON* fresh = cJSON_CreateObject();
    cJSON_AddItemToObject(g_root, ns.c_str(), fresh);
    return fresh;
}

// Caller holds g_mutex.
bool FlushLocked() {
    if (!g_dirty || g_root == nullptr) {
        return true;
    }
    MakeParentDirs(g_path);
    char* text = cJSON_Print(g_root);
    if (text == nullptr) {
        return false;
    }
    std::string tmp = g_path + ".tmp";
    bool ok = false;
    FILE* f = fopen(tmp.c_str(), "wb");
    if (f != nullptr) {
        size_t len = strlen(text);
        ok = fwrite(text, 1, len, f) == len;
        // Settings hold the device identity and the server endpoint. Losing
        // them to a power cut mid-write would force a re-activation, so pay for
        // the fsync before the rename.
        if (ok) {
            ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
        }
        fclose(f);
    }
    if (ok) {
        ok = rename(tmp.c_str(), g_path.c_str()) == 0;
    }
    if (!ok) {
        unlink(tmp.c_str());
        C1XZ_LOGE(TAG, "failed to write %s", g_path.c_str());
    } else {
        g_dirty = false;
    }
    cJSON_free(text);
    return ok;
}

}  // namespace

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {}

Settings::~Settings() {
    if (read_write_ && dirty_) {
        std::lock_guard<std::mutex> lock(g_mutex);
        FlushLocked();
    }
}

std::string Settings::FilePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_path.empty()) {
        g_path = DefaultPath();
    }
    return g_path;
}

void Settings::SetFilePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    FlushLocked();
    if (g_root != nullptr) {
        cJSON_Delete(g_root);
        g_root = nullptr;
    }
    g_loaded = false;
    g_dirty = false;
    g_path = path;
}

bool Settings::Flush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return FlushLocked();
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    cJSON* ns = NamespaceLocked(ns_, false);
    if (ns == nullptr) {
        return default_value;
    }
    cJSON* item = cJSON_GetObjectItem(ns, key.c_str());
    if (item == nullptr || !cJSON_IsString(item) || item->valuestring == nullptr) {
        return default_value;
    }
    return item->valuestring;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    if (!read_write_) {
        C1XZ_LOGW(TAG, "%s.%s written through a read-only handle, ignored", ns_.c_str(),
                  key.c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    cJSON* ns = NamespaceLocked(ns_, true);
    cJSON_DeleteItemFromObject(ns, key.c_str());
    cJSON_AddStringToObject(ns, key.c_str(), value.c_str());
    g_dirty = true;
    dirty_ = true;
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    cJSON* ns = NamespaceLocked(ns_, false);
    if (ns == nullptr) {
        return default_value;
    }
    cJSON* item = cJSON_GetObjectItem(ns, key.c_str());
    if (item == nullptr) {
        return default_value;
    }
    if (cJSON_IsNumber(item)) {
        return static_cast<int32_t>(item->valuedouble);
    }
    // Tolerate a number that was stored as a string by an older build.
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        char* end = nullptr;
        long parsed = strtol(item->valuestring, &end, 10);
        if (end != item->valuestring) {
            return static_cast<int32_t>(parsed);
        }
    }
    return default_value;
}

void Settings::SetInt(const std::string& key, int32_t value) {
    if (!read_write_) {
        C1XZ_LOGW(TAG, "%s.%s written through a read-only handle, ignored", ns_.c_str(),
                  key.c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    cJSON* ns = NamespaceLocked(ns_, true);
    cJSON_DeleteItemFromObject(ns, key.c_str());
    cJSON_AddNumberToObject(ns, key.c_str(), value);
    g_dirty = true;
    dirty_ = true;
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    cJSON* ns = NamespaceLocked(ns_, false);
    if (ns == nullptr) {
        return default_value;
    }
    cJSON* item = cJSON_GetObjectItem(ns, key.c_str());
    if (item == nullptr) {
        return default_value;
    }
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) != 0;
    }
    if (cJSON_IsNumber(item)) {
        return item->valuedouble != 0;
    }
    return default_value;
}

void Settings::SetBool(const std::string& key, bool value) {
    if (!read_write_) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    cJSON* ns = NamespaceLocked(ns_, true);
    cJSON_DeleteItemFromObject(ns, key.c_str());
    cJSON_AddBoolToObject(ns, key.c_str(), value);
    g_dirty = true;
    dirty_ = true;
}

std::vector<std::string> Settings::Keys() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::string> keys;
    cJSON* ns = NamespaceLocked(ns_, false);
    if (ns == nullptr) {
        return keys;
    }
    for (cJSON* item = ns->child; item != nullptr; item = item->next) {
        if (item->string != nullptr) {
            keys.emplace_back(item->string);
        }
    }
    return keys;
}

void Settings::EraseKey(const std::string& key) {
    if (!read_write_) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    cJSON* ns = NamespaceLocked(ns_, false);
    if (ns == nullptr) {
        return;
    }
    cJSON_DeleteItemFromObject(ns, key.c_str());
    g_dirty = true;
    dirty_ = true;
}

void Settings::EraseAll() {
    if (!read_write_) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    cJSON_DeleteItemFromObject(g_root, ns_.c_str());
    g_dirty = true;
    dirty_ = true;
}
