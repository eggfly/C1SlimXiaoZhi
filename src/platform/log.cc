#include "platform/log.h"

#include <cstdarg>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace c1xz {
namespace {

std::mutex g_mutex;
LogLevel g_level = LogLevel::kInfo;
FILE* g_file = nullptr;
std::string g_file_path;
long g_file_max = 0;

char LevelChar(LogLevel level) {
    switch (level) {
        case LogLevel::kError: return 'E';
        case LogLevel::kWarn: return 'W';
        case LogLevel::kInfo: return 'I';
        case LogLevel::kDebug: return 'D';
        case LogLevel::kVerbose: return 'V';
        default: return '?';
    }
}

}  // namespace

void LogSetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

LogLevel LogGetLevel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

bool LogSetFile(const char* path, long max_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        fclose(g_file);
        g_file = nullptr;
        g_file_path.clear();
    }
    if (path == nullptr) {
        return true;
    }
    g_file = fopen(path, "a");
    if (g_file == nullptr) {
        return false;
    }
    g_file_path = path;
    g_file_max = max_bytes;
    return true;
}

void LogWrite(LogLevel level, const char* tag, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (level > g_level) {
        return;
    }

    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm tm_buf{};
    localtime_r(&ts.tv_sec, &tm_buf);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03ld", tm_buf.tm_hour, tm_buf.tm_min,
             tm_buf.tm_sec, ts.tv_nsec / 1000000);

    char body[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    fprintf(stderr, "%s %c %s: %s\n", stamp, LevelChar(level), tag, body);
    fflush(stderr);

    if (g_file != nullptr) {
        fprintf(g_file, "%s %c %s: %s\n", stamp, LevelChar(level), tag, body);
        fflush(g_file);
        if (g_file_max > 0 && ftell(g_file) > g_file_max) {
            // Truncate rather than rotate: /storage is large but a runaway log
            // still should not grow without bound, and we never need history.
            freopen(g_file_path.c_str(), "w", g_file);
        }
    }
}

}  // namespace c1xz
