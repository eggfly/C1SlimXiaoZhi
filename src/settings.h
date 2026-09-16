#ifndef C1XZ_SETTINGS_H
#define C1XZ_SETTINGS_H

#include <cstdint>
#include <string>
#include <vector>

// Persistent key/value store with the same API and namespace names as the
// upstream NVS-backed Settings class, so ported code keeps working unchanged.
//
// Everything lives in one JSON file written atomically (temp file + rename).
// Namespaces are top level objects: "wifi", "websocket", "mqtt", "board",
// "assets", "display", "vendor", "network". Those names are persistent API;
// changing one needs a migration, exactly as on the device's NVS.
class Settings {
public:
    explicit Settings(const std::string& ns, bool read_write = false);
    ~Settings();

    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    std::string GetString(const std::string& key, const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    void SetInt(const std::string& key, int32_t value);
    bool GetBool(const std::string& key, bool default_value = false);
    void SetBool(const std::string& key, bool value);
    std::vector<std::string> Keys();
    void EraseKey(const std::string& key);
    void EraseAll();

    // Absolute path of the backing file. Defaults to
    // /storage/c1/xiaozhi/settings.json, overridable with $C1XZ_SETTINGS.
    static std::string FilePath();
    static void SetFilePath(const std::string& path);

    // Forces a flush of pending writes. Normally done by the destructor.
    static bool Flush();

private:
    std::string ns_;
    bool read_write_;
    bool dirty_ = false;
};

#endif  // C1XZ_SETTINGS_H
