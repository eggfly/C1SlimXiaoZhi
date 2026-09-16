#ifndef C1XZ_OTA_H
#define C1XZ_OTA_H

#include <functional>
#include <string>
#include <vector>

namespace c1xz {
class HttpClient;
}

// Ported from xiaozhi-esp32 main/ota.*.
//
// Talks to the xiaozhi OTA endpoint: reports what this device is, receives the
// transport configuration (websocket or mqtt), the server time, an activation
// challenge when the device is not yet bound to an account, and any available
// update.
class Ota {
public:
    Ota();
    ~Ota() = default;

    enum class Result { kOk, kNetworkError, kProtocolError, kHttpError };

    // POSTs the system information document and applies whatever comes back.
    Result CheckVersion();

    // Polls the activation endpoint. kOk means the user finished entering the
    // code; kHttpError with status 202 means "still waiting".
    enum class ActivationResult { kActivated, kPending, kFailed };
    ActivationResult Activate();

    bool HasActivationCode() const { return has_activation_code_; }
    bool HasActivationChallenge() const { return has_activation_challenge_; }
    bool HasNewVersion() const { return has_new_version_; }
    bool HasWebsocketConfig() const { return has_websocket_config_; }
    bool HasMqttConfig() const { return has_mqtt_config_; }
    bool HasServerTime() const { return has_server_time_; }

    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetLastError() const { return last_error_; }
    int GetHttpStatus() const { return http_status_; }

    std::string GetCheckVersionUrl();

    // Downloads a new executable, verifies it, swaps it in and re-execs. Only
    // returns when the upgrade failed: on success the process is replaced.
    bool StartUpgrade(const std::function<void(int percent, size_t bytes_per_second)>& progress);

private:
    std::string activation_message_;
    std::string activation_code_;
    std::string activation_challenge_;
    std::string serial_number_;
    std::string hmac_key_;  // hex, from settings; empty unless self-provisioned
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string last_error_;

    bool has_new_version_ = false;
    bool has_websocket_config_ = false;
    bool has_mqtt_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_activation_challenge_ = false;
    bool has_serial_number_ = false;
    int activation_timeout_ms_ = 30000;
    int http_status_ = 0;

    void ApplyHeaders(c1xz::HttpClient* http);
    std::string GetActivationPayload();
    static bool ParseVersion(const std::string& text, std::vector<int>* parts);
    static bool IsNewerVersion(const std::string& current, const std::string& candidate);
};

#endif  // C1XZ_OTA_H
