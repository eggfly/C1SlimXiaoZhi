#ifndef C1XZ_NET_HTTP_CLIENT_H
#define C1XZ_NET_HTTP_CLIENT_H

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace c1xz {

struct Url {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path;  // includes the query string
    bool secure = false;

    static bool Parse(const std::string& text, Url* out);
    std::string Origin() const;
};

// Minimal blocking HTTP/1.1 client: the verbs, headers and body handling the
// OTA check, the asset download and the MCP web tools actually need.
//
// Supports Content-Length and chunked responses, and follows redirects.
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    void SetHeader(const std::string& key, const std::string& value);
    void SetContent(std::string body);
    void SetTimeout(int milliseconds);
    // Disables certificate checking. Only for a self-signed local server or the
    // one-shot time fetch made before the clock is known to be sane.
    void SetVerifyPeer(bool verify);
    void SetMaxRedirects(int count);

    // Sends the request and reads the response headers. The body is left for
    // ReadAll or ReadBody.
    bool Open(const std::string& method, const std::string& url);

    int status_code() const { return status_code_; }
    std::string GetResponseHeader(const std::string& key) const;
    size_t content_length() const { return content_length_; }

    // Reads the whole body into memory. Refuses bodies over max_bytes so a
    // hostile or broken server cannot exhaust the device's 50 MiB of RAM.
    std::string ReadAll(size_t max_bytes = 8 * 1024 * 1024);

    // Streams the body out. The callback returning false aborts the transfer.
    bool ReadBody(const std::function<bool(const char* data, size_t len)>& sink);

    void Close();

    const std::string& error() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::map<std::string, std::string> request_headers_;
    std::string request_body_;
    int timeout_ms_ = 20000;
    bool verify_peer_ = true;
    int max_redirects_ = 5;

    int status_code_ = 0;
    size_t content_length_ = 0;
    bool chunked_ = false;
    std::map<std::string, std::string> response_headers_;
    std::string error_;

    bool OpenOnce(const std::string& method, const Url& url, std::string* redirect_to);
};

}  // namespace c1xz

#endif  // C1XZ_NET_HTTP_CLIENT_H
