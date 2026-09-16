// c1xiaozhi: the xiaozhi voice assistant on the 快易典 C1 Slim / MP-D261.
//
// A single static MIPS binary. It expects to be started by a launcher that has
// already brought up Wi-Fi, and it hands the screen back when it exits.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <unistd.h>

#include "application.h"
#include "platform/log.h"
#include "settings.h"
#include "system_info.h"

namespace {

volatile sig_atomic_t g_signal = 0;

void OnSignal(int number) { g_signal = number; }

void PrintUsage(const char* argv0) {
    printf(
        "usage: %s [options]\n"
        "\n"
        "  -v, --verbose        debug logging\n"
        "  -q, --quiet          errors only\n"
        "      --log FILE       also write the log to FILE (capped at 1 MiB)\n"
        "      --settings FILE  settings file (default /storage/c1/xiaozhi/settings.json)\n"
        "      --ota-url URL    override the OTA endpoint and persist it\n"
        "      --version        print the version and exit\n"
        "  -h, --help           this message\n"
        "\n"
        "environment:\n"
        "  C1XZ_FONT      bitmap font produced by tools/build_font.py\n"
        "  C1XZ_SOUNDS    directory of Ogg Opus prompt sounds\n"
        "  C1XZ_KEYMAP    key code overrides, e.g. \"28=talk,1=cancel\"\n"
        "  C1XZ_SETTINGS  settings file path\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string log_file;
    std::string ota_url;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "--version") {
            printf("c1xiaozhi %s (%s)\n", C1XZ_VERSION, SystemInfo::GetBoardName());
            return 0;
        }
        if (arg == "-v" || arg == "--verbose") {
            c1xz::LogSetLevel(c1xz::LogLevel::kDebug);
        } else if (arg == "-q" || arg == "--quiet") {
            c1xz::LogSetLevel(c1xz::LogLevel::kError);
        } else if (arg == "--log" && i + 1 < argc) {
            log_file = argv[++i];
        } else if (arg == "--settings" && i + 1 < argc) {
            Settings::SetFilePath(argv[++i]);
        } else if (arg == "--ota-url" && i + 1 < argc) {
            ota_url = argv[++i];
        } else {
            fprintf(stderr, "unknown option: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    if (!log_file.empty()) {
        c1xz::LogSetFile(log_file.c_str(), 1024 * 1024);
    }
    if (!ota_url.empty()) {
        Settings settings("wifi", true);
        settings.SetString("ota_url", ota_url);
    }

    // SIGPIPE would otherwise kill the process when the server goes away
    // mid-write; the socket code already handles the error return.
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    C1XZ_LOGI("main", "c1xiaozhi %s starting on %s (%s)", C1XZ_VERSION,
              SystemInfo::GetBoardName(), SystemInfo::GetKernelVersion().c_str());
    C1XZ_LOGI("main", "device id %s", SystemInfo::GetMacAddress().c_str());
    if (!SystemInfo::IsClockPlausible()) {
        C1XZ_LOGW("main",
                  "the system clock is not set; TLS will fail until the server supplies the time");
    }

    auto& app = Application::GetInstance();
    if (!app.Start()) {
        C1XZ_LOGE("main", "startup failed");
        return 1;
    }

    // Turn a signal into an orderly shutdown on the event loop, so the screen
    // and the paused factory UI are restored.
    std::thread([&app]() {
        while (g_signal == 0) {
            usleep(100 * 1000);
        }
        C1XZ_LOGI("main", "caught signal %d", static_cast<int>(g_signal));
        app.Schedule([&app]() { app.Quit(); });
    }).detach();

    app.Run();
    app.Quit();
    C1XZ_LOGI("main", "goodbye");
    return 0;
}
