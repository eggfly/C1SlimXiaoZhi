#ifndef C1XZ_PLATFORM_LOG_H
#define C1XZ_PLATFORM_LOG_H

// Drop-in replacement for the esp_log.h macros the upstream xiaozhi-esp32 code
// uses, so ported files keep their original logging statements.

#include <cstdio>

namespace c1xz {

enum class LogLevel { kNone = 0, kError, kWarn, kInfo, kDebug, kVerbose };

// Writes to stderr, and to the log file when one was opened. Thread safe.
void LogWrite(LogLevel level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

void LogSetLevel(LogLevel level);
LogLevel LogGetLevel();

// Opens a size-capped log file. Passing nullptr closes it. The file is
// truncated when it exceeds max_bytes so a long run cannot fill /storage.
bool LogSetFile(const char* path, long max_bytes);

}  // namespace c1xz

#define C1XZ_LOGE(tag, fmt, ...) ::c1xz::LogWrite(::c1xz::LogLevel::kError, tag, fmt, ##__VA_ARGS__)
#define C1XZ_LOGW(tag, fmt, ...) ::c1xz::LogWrite(::c1xz::LogLevel::kWarn, tag, fmt, ##__VA_ARGS__)
#define C1XZ_LOGI(tag, fmt, ...) ::c1xz::LogWrite(::c1xz::LogLevel::kInfo, tag, fmt, ##__VA_ARGS__)
#define C1XZ_LOGD(tag, fmt, ...) ::c1xz::LogWrite(::c1xz::LogLevel::kDebug, tag, fmt, ##__VA_ARGS__)
#define C1XZ_LOGV(tag, fmt, ...) ::c1xz::LogWrite(::c1xz::LogLevel::kVerbose, tag, fmt, ##__VA_ARGS__)

// Aliases so upstream sources compile unchanged.
#define ESP_LOGE C1XZ_LOGE
#define ESP_LOGW C1XZ_LOGW
#define ESP_LOGI C1XZ_LOGI
#define ESP_LOGD C1XZ_LOGD
#define ESP_LOGV C1XZ_LOGV

#endif  // C1XZ_PLATFORM_LOG_H
