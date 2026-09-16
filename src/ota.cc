#include "ota.h"

#include <cJSON.h>
#include <mbedtls/md.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "boards/board.h"
#include "net/http_client.h"
#include "platform/log.h"
#include "settings.h"
#include "system_info.h"

#define TAG "Ota"

namespace {

// Same default as upstream, so a device pointed at the official service works
// with no configuration at all.
const char kDefaultOtaUrl[] = "https://api.tenclass.net/xiaozhi/ota/";

std::string HexEncode(const unsigned char* data, size_t length) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0f]);
    }
    return out;
}

bool HexDecode(const std::string& hex, std::vector<unsigned char>* out) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out->clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = value(hex[i]);
        int lo = value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out->push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return true;
}

std::string SelfPath() {
    char buffer[4096];
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        return {};
    }
    buffer[n] = '\0';
    return buffer;
}

}  // namespace

Ota::Ota() {
    current_version_ = C1XZ_VERSION;
    Settings settings("device", false);
    serial_number_ = settings.GetString("serial_number");
    hmac_key_ = settings.GetString("hmac_key");
    has_serial_number_ = !serial_number_.empty() && !hmac_key_.empty();
    if (has_serial_number_) {
        // The ESP32 keeps this key in an eFuse the CPU cannot read back. Here
        // it is a file on a writable partition, so it is only as strong as
        // physical possession of the device. Fine for a self-hosted server,
        // not equivalent to hardware-backed attestation.
        C1XZ_LOGI(TAG, "using serial number %s for activation", serial_number_.c_str());
    }
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = kDefaultOtaUrl;
    }
    return url;
}

void Ota::ApplyHeaders(c1xz::HttpClient* http) {
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_);
    }
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader("Accept-Language", "zh-CN");
    http->SetHeader("Content-Type", "application/json");
}

bool Ota::ParseVersion(const std::string& text, std::vector<int>* parts) {
    parts->clear();
    size_t start = 0;
    while (start <= text.size()) {
        size_t dot = text.find('.', start);
        std::string piece =
            dot == std::string::npos ? text.substr(start) : text.substr(start, dot - start);
        if (piece.empty()) {
            return false;
        }
        char* end = nullptr;
        long value = strtol(piece.c_str(), &end, 10);
        if (end == piece.c_str() || value < 0) {
            return false;
        }
        parts->push_back(static_cast<int>(value));
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return !parts->empty();
}

bool Ota::IsNewerVersion(const std::string& current, const std::string& candidate) {
    std::vector<int> a;
    std::vector<int> b;
    if (!ParseVersion(current, &a) || !ParseVersion(candidate, &b)) {
        C1XZ_LOGW(TAG, "cannot compare versions '%s' and '%s'", current.c_str(),
                  candidate.c_str());
        return false;
    }
    size_t count = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < count; ++i) {
        int left = i < a.size() ? a[i] : 0;
        int right = i < b.size() ? b[i] : 0;
        if (right > left) {
            return true;
        }
        if (right < left) {
            return false;
        }
    }
    return false;
}

Ota::Result Ota::CheckVersion() {
    last_error_.clear();
    http_status_ = 0;
    has_new_version_ = false;
    has_activation_code_ = false;
    has_activation_challenge_ = false;
    has_websocket_config_ = false;
    has_mqtt_config_ = false;
    has_server_time_ = false;

    std::string url = GetCheckVersionUrl();
    if (url.size() < 10) {
        last_error_ = "OTA URL is not configured";
        return Result::kProtocolError;
    }

    c1xz::HttpClient http;
    ApplyHeaders(&http);
    // A device whose clock is wrong cannot validate any certificate. Rather
    // than silently trusting anything, note it: the caller shows the user a
    // clear message and the OTA response carries a server_time we then apply.
    if (!SystemInfo::IsClockPlausible()) {
        C1XZ_LOGW(TAG, "system clock looks wrong; TLS validation is likely to fail");
    }
    http.SetContent(Board::GetInstance().GetSystemInfoJson());

    if (!http.Open("POST", url)) {
        last_error_ = http.error();
        C1XZ_LOGE(TAG, "OTA request failed: %s", last_error_.c_str());
        return Result::kNetworkError;
    }
    http_status_ = http.status_code();
    if (http_status_ != 200) {
        last_error_ = "server returned HTTP " + std::to_string(http_status_);
        C1XZ_LOGE(TAG, "%s", last_error_.c_str());
        return Result::kHttpError;
    }

    std::string body = http.ReadAll(1024 * 1024);
    http.Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        last_error_ = "OTA response is not valid JSON";
        return Result::kProtocolError;
    }

    cJSON* activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout)) {
            activation_timeout_ms_ = timeout->valueint;
        }
    }

    cJSON* websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON* item = cJSON_GetObjectItem(websocket, "url");
        if (cJSON_IsString(item)) {
            settings.SetString("url", item->valuestring);
        }
        item = cJSON_GetObjectItem(websocket, "token");
        if (cJSON_IsString(item)) {
            settings.SetString("token", item->valuestring);
        }
        item = cJSON_GetObjectItem(websocket, "version");
        if (cJSON_IsNumber(item)) {
            settings.SetInt("version", item->valueint);
        }
        has_websocket_config_ = true;
    }

    cJSON* mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        for (cJSON* item = mqtt->child; item != nullptr; item = item->next) {
            if (item->string == nullptr) {
                continue;
            }
            if (cJSON_IsString(item)) {
                settings.SetString(item->string, item->valuestring);
            } else if (cJSON_IsNumber(item)) {
                settings.SetInt(item->string, item->valueint);
            }
        }
        has_mqtt_config_ = true;
    }

    cJSON* server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON* timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON* offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        if (cJSON_IsNumber(timestamp)) {
            // The device has no RTC battery, so after a long power-off the
            // clock is wrong and certificate validity checks fail. Trust the
            // server's time only to the extent of setting the clock.
            double milliseconds = timestamp->valuedouble;
            if (cJSON_IsNumber(offset)) {
                milliseconds += offset->valuedouble * 60000.0;
            }
            timeval tv{};
            tv.tv_sec = static_cast<time_t>(milliseconds / 1000);
            tv.tv_usec = static_cast<suseconds_t>(
                (milliseconds - static_cast<double>(tv.tv_sec) * 1000) * 1000);
            if (settimeofday(&tv, nullptr) == 0) {
                C1XZ_LOGI(TAG, "clock set from the server");
            }
            has_server_time_ = true;
        }
    }

    cJSON* firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON* version = cJSON_GetObjectItem(firmware, "version");
        cJSON* url_item = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        if (cJSON_IsString(url_item)) {
            firmware_url_ = url_item->valuestring;
        }
        cJSON* force = cJSON_GetObjectItem(firmware, "force");
        bool forced = cJSON_IsNumber(force) ? force->valueint != 0 : cJSON_IsTrue(force);
        if (!firmware_version_.empty() && !firmware_url_.empty()) {
            has_new_version_ = forced || IsNewerVersion(current_version_, firmware_version_);
            if (has_new_version_) {
                C1XZ_LOGI(TAG, "update available: %s -> %s", current_version_.c_str(),
                          firmware_version_.c_str());
            }
        }
    }

    cJSON_Delete(root);
    return Result::kOk;
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::vector<unsigned char> key;
    if (!HexDecode(hmac_key_, &key) || key.empty()) {
        C1XZ_LOGE(TAG, "hmac_key in settings is not valid hex");
        return "{}";
    }

    unsigned char digest[32];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr ||
        mbedtls_md_hmac(info, key.data(), key.size(),
                        reinterpret_cast<const unsigned char*>(activation_challenge_.data()),
                        activation_challenge_.size(), digest) != 0) {
        C1XZ_LOGE(TAG, "HMAC calculation failed");
        return "{}";
    }

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", HexEncode(digest, sizeof(digest)).c_str());
    char* text = cJSON_PrintUnformatted(payload);
    std::string json(text != nullptr ? text : "{}");
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(payload);
    return json;
}

Ota::ActivationResult Ota::Activate() {
    if (!has_activation_challenge_) {
        C1XZ_LOGW(TAG, "no activation challenge to answer");
        return ActivationResult::kFailed;
    }

    std::string url = GetCheckVersionUrl();
    if (url.empty()) {
        return ActivationResult::kFailed;
    }
    url += url.back() == '/' ? "activate" : "/activate";

    c1xz::HttpClient http;
    ApplyHeaders(&http);
    http.SetContent(GetActivationPayload());

    if (!http.Open("POST", url)) {
        last_error_ = http.error();
        return ActivationResult::kFailed;
    }
    http_status_ = http.status_code();
    if (http_status_ == 202) {
        // The server is still waiting for the user to type the code.
        return ActivationResult::kPending;
    }
    if (http_status_ != 200) {
        last_error_ = "activation returned HTTP " + std::to_string(http_status_);
        C1XZ_LOGE(TAG, "%s: %s", last_error_.c_str(), http.ReadAll(4096).c_str());
        return ActivationResult::kFailed;
    }
    C1XZ_LOGI(TAG, "device activated");
    return ActivationResult::kActivated;
}

bool Ota::StartUpgrade(const std::function<void(int, size_t)>& progress) {
    if (firmware_url_.empty()) {
        return false;
    }
    std::string self = SelfPath();
    if (self.empty()) {
        C1XZ_LOGE(TAG, "cannot determine my own path; refusing to self-update");
        return false;
    }
    std::string staged = self + ".new";

    C1XZ_LOGI(TAG, "downloading %s", firmware_url_.c_str());
    c1xz::HttpClient http;
    http.SetHeader("User-Agent", SystemInfo::GetUserAgent());
    if (!http.Open("GET", firmware_url_) || http.status_code() != 200) {
        C1XZ_LOGE(TAG, "download failed: HTTP %d %s", http.status_code(), http.error().c_str());
        return false;
    }

    FILE* out = fopen(staged.c_str(), "wb");
    if (out == nullptr) {
        C1XZ_LOGE(TAG, "cannot write %s", staged.c_str());
        return false;
    }

    size_t total = http.content_length();
    size_t written = 0;
    int64_t started = 0;
    bool ok = http.ReadBody([&](const char* data, size_t len) {
        if (fwrite(data, 1, len, out) != len) {
            return false;
        }
        written += len;
        if (progress) {
            int percent = total > 0 ? static_cast<int>(written * 100 / total) : 0;
            progress(percent, 0);
        }
        (void)started;
        return true;
    });
    fclose(out);

    if (!ok || written == 0 || (total > 0 && written != total)) {
        C1XZ_LOGE(TAG, "download incomplete: %zu of %zu bytes", written, total);
        unlink(staged.c_str());
        return false;
    }

    // Refuse anything that is not an ELF for this machine. Without a signature
    // we cannot verify the publisher, but we can at least refuse to install a
    // file that cannot possibly be our executable.
    unsigned char header[20] = {0};
    FILE* check = fopen(staged.c_str(), "rb");
    if (check == nullptr || fread(header, 1, sizeof(header), check) != sizeof(header)) {
        if (check != nullptr) {
            fclose(check);
        }
        unlink(staged.c_str());
        return false;
    }
    fclose(check);
    const bool is_elf = header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' &&
                        header[3] == 'F' && header[4] == 1 /* 32-bit */ &&
                        header[5] == 1 /* little endian */;
    const uint16_t machine = static_cast<uint16_t>(header[18] | (header[19] << 8));
    if (!is_elf || machine != 8 /* EM_MIPS */) {
        C1XZ_LOGE(TAG, "downloaded file is not a 32-bit little-endian MIPS ELF");
        unlink(staged.c_str());
        return false;
    }

    chmod(staged.c_str(), 0755);
    if (rename(staged.c_str(), self.c_str()) != 0) {
        C1XZ_LOGE(TAG, "cannot replace %s", self.c_str());
        unlink(staged.c_str());
        return false;
    }

    C1XZ_LOGI(TAG, "update installed, restarting into %s", firmware_version_.c_str());
    Settings::Flush();
    // Re-exec rather than reboot: the device's launcher would have to bring us
    // back, and a reboot in the middle of a conversation is far more disruptive
    // than a restart.
    char* const argv[] = {const_cast<char*>(self.c_str()), nullptr};
    execv(self.c_str(), argv);
    C1XZ_LOGE(TAG, "execv failed after upgrade; the new binary is in place");
    return false;
}
