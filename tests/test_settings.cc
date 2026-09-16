#include "settings.h"
#include "test_framework.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

std::string TempSettingsPath() {
    return "/tmp/c1xz-test-settings-" + std::to_string(getpid()) + ".json";
}

}  // namespace

TEST(SettingsRoundTripAcrossNamespaces) {
    std::string path = TempSettingsPath();
    unlink(path.c_str());
    Settings::SetFilePath(path);

    {
        Settings websocket("websocket", true);
        websocket.SetString("url", "wss://example.com/ws");
        websocket.SetInt("version", 3);
        Settings board("board", true);
        board.SetString("uuid", "11111111-2222-4333-8444-555555555555");
    }
    Settings::Flush();

    // Drop the in-memory copy so the values must come back from disk.
    Settings::SetFilePath(path);
    {
        Settings websocket("websocket", false);
        CHECK_STREQ(websocket.GetString("url"), "wss://example.com/ws");
        CHECK_EQ(websocket.GetInt("version"), 3);
        Settings board("board", false);
        CHECK_STREQ(board.GetString("uuid"), "11111111-2222-4333-8444-555555555555");
        // A namespace that was never written returns the default.
        Settings mqtt("mqtt", false);
        CHECK_STREQ(mqtt.GetString("endpoint", "none"), "none");
    }
    unlink(path.c_str());
}

TEST(ReadOnlyHandleDoesNotWrite) {
    std::string path = TempSettingsPath();
    unlink(path.c_str());
    Settings::SetFilePath(path);

    {
        Settings settings("audio", false);
        settings.SetInt("volume", 99);
    }
    {
        Settings settings("audio", false);
        CHECK_EQ(settings.GetInt("volume", 70), 70);
    }
    unlink(path.c_str());
}

TEST(IntStoredAsStringStillParses) {
    // Older builds wrote some numbers as strings; reading must tolerate it.
    std::string path = TempSettingsPath();
    unlink(path.c_str());
    FILE* f = fopen(path.c_str(), "w");
    CHECK(f != nullptr);
    fputs("{\"websocket\":{\"version\":\"2\"}}", f);
    fclose(f);

    Settings::SetFilePath(path);
    Settings settings("websocket", false);
    CHECK_EQ(settings.GetInt("version"), 2);
    unlink(path.c_str());
}

TEST(CorruptFileDoesNotLoseFutureWrites) {
    std::string path = TempSettingsPath();
    unlink(path.c_str());
    FILE* f = fopen(path.c_str(), "w");
    CHECK(f != nullptr);
    fputs("{ this is not json", f);
    fclose(f);

    Settings::SetFilePath(path);
    {
        Settings settings("board", true);
        settings.SetString("uuid", "recovered");
    }
    Settings::Flush();
    Settings::SetFilePath(path);
    Settings settings("board", false);
    CHECK_STREQ(settings.GetString("uuid"), "recovered");
    unlink(path.c_str());
}

TEST(EraseRemovesKey) {
    std::string path = TempSettingsPath();
    unlink(path.c_str());
    Settings::SetFilePath(path);
    {
        Settings settings("audio", true);
        settings.SetInt("volume", 55);
        settings.EraseKey("volume");
    }
    Settings::Flush();
    Settings::SetFilePath(path);
    Settings settings("audio", false);
    CHECK_EQ(settings.GetInt("volume", 70), 70);
    unlink(path.c_str());
}
