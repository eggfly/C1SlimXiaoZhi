#include "net/http_client.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "net/tls_socket.h"
#include "platform/log.h"

#define TAG "Http"

namespace c1xz {
namespace {

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return value;
}

std::string Trim(const std::string& value) {
    size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

}  // namespace

bool Url::Parse(const std::string& text, Url* out) {
    size_t scheme_end = text.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    out->scheme = ToLower(text.substr(0, scheme_end));
    if (out->scheme == "https" || out->scheme == "wss") {
        out->secure = true;
        out->port = 443;
    } else if (out->scheme == "http" || out->scheme == "ws") {
        out->secure = false;
        out->port = 80;
    } else {
        return false;
    }

    size_t host_begin = scheme_end + 3;
    size_t path_begin = text.find('/', host_begin);
    std::string authority = path_begin == std::string::npos
                                ? text.substr(host_begin)
                                : text.substr(host_begin, path_begin - host_begin);
    out->path = path_begin == std::string::npos ? "/" : text.substr(path_begin);
    if (out->path.empty()) {
        out->path = "/";
    }

    // Strip any userinfo; we never send credentials in the URL.
    size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }

    if (!authority.empty() && authority[0] == '[') {  // IPv6 literal
        size_t close = authority.find(']');
        if (close == std::string::npos) {
            return false;
        }
        out->host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            out->port = atoi(authority.c_str() + close + 2);
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            out->host = authority.substr(0, colon);
            out->port = atoi(authority.c_str() + colon + 1);
        } else {
            out->host = authority;
        }
    }
    return !out->host.empty() && out->port > 0 && out->port < 65536;
}

std::string Url::Origin() const {
    return scheme + "://" + host + ":" + std::to_string(port);
}

struct HttpClient::Impl {
    TlsSocket socket;
    std::string buffer;  // bytes read past the headers
    size_t body_read = 0;
};

HttpClient::HttpClient() : impl_(new Impl()) {}
HttpClient::~HttpClient() { Close(); }

void HttpClient::SetHeader(const std::string& key, const std::string& value) {
    request_headers_[key] = value;
}

void HttpClient::SetContent(std::string body) { request_body_ = std::move(body); }
void HttpClient::SetTimeout(int milliseconds) { timeout_ms_ = milliseconds; }
void HttpClient::SetVerifyPeer(bool verify) { verify_peer_ = verify; }
void HttpClient::SetMaxRedirects(int count) { max_redirects_ = count; }

std::string HttpClient::GetResponseHeader(const std::string& key) const {
    auto it = response_headers_.find(ToLower(key));
    return it == response_headers_.end() ? std::string() : it->second;
}

bool HttpClient::Open(const std::string& method, const std::string& url) {
    std::string current = url;
    for (int redirect = 0; redirect <= max_redirects_; ++redirect) {
        Url parsed;
        if (!Url::Parse(current, &parsed)) {
            error_ = "malformed URL: " + current;
            C1XZ_LOGE(TAG, "%s", error_.c_str());
            return false;
        }
        std::string next;
        if (!OpenOnce(method, parsed, &next)) {
            return false;
        }
        if (next.empty()) {
            return true;
        }
        // Resolve a relative Location against the current origin.
        if (next.find("://") == std::string::npos) {
            next = next[0] == '/' ? parsed.Origin() + next : parsed.Origin() + "/" + next;
        }
        C1XZ_LOGI(TAG, "redirect %d -> %s", status_code_, next.c_str());
        current = next;
        Close();
    }
    error_ = "too many redirects";
    return false;
}

bool HttpClient::OpenOnce(const std::string& method, const Url& url, std::string* redirect_to) {
    redirect_to->clear();
    response_headers_.clear();
    status_code_ = 0;
    content_length_ = 0;
    chunked_ = false;
    impl_.reset(new Impl());

    TlsSocket::Options options;
    options.host = url.host;
    options.port = url.port;
    options.use_tls = url.secure;
    options.verify_peer = verify_peer_;
    options.connect_timeout_ms = timeout_ms_;
    options.io_timeout_ms = timeout_ms_;
    if (!impl_->socket.Connect(options)) {
        error_ = impl_->socket.last_error();
        return false;
    }

    std::string request = method + " " + url.path + " HTTP/1.1\r\n";
    request += "Host: " + url.host;
    if ((url.secure && url.port != 443) || (!url.secure && url.port != 80)) {
        request += ":" + std::to_string(url.port);
    }
    request += "\r\n";
    // No keep-alive: every request here is one-shot, and closing is the
    // simplest reliable way to know a body without Content-Length ended.
    request += "Connection: close\r\n";
    bool has_length = false;
    for (const auto& header : request_headers_) {
        if (ToLower(header.first) == "content-length") {
            has_length = true;
        }
        request += header.first + ": " + header.second + "\r\n";
    }
    if (!request_body_.empty() && !has_length) {
        request += "Content-Length: " + std::to_string(request_body_.size()) + "\r\n";
    }
    request += "\r\n";
    request += request_body_;

    if (impl_->socket.Write(request.data(), request.size()) < 0) {
        error_ = "failed to send request: " + impl_->socket.last_error();
        return false;
    }

    // Read until the end of the header block.
    char chunk[2048];
    size_t header_end = std::string::npos;
    while (true) {
        header_end = impl_->buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            break;
        }
        if (impl_->buffer.size() > 64 * 1024) {
            error_ = "response headers too large";
            return false;
        }
        int n = impl_->socket.Read(chunk, sizeof(chunk));
        if (n <= 0) {
            error_ = n == TlsSocket::kTimeout ? "timed out reading response headers"
                                              : "connection closed before headers completed";
            C1XZ_LOGE(TAG, "%s", error_.c_str());
            return false;
        }
        impl_->buffer.append(chunk, static_cast<size_t>(n));
    }

    std::string head = impl_->buffer.substr(0, header_end);
    impl_->buffer.erase(0, header_end + 4);

    size_t line_end = head.find("\r\n");
    std::string status_line = head.substr(0, line_end);
    if (status_line.compare(0, 5, "HTTP/") != 0) {
        error_ = "not an HTTP response";
        return false;
    }
    size_t code_begin = status_line.find(' ');
    if (code_begin == std::string::npos) {
        error_ = "malformed status line";
        return false;
    }
    status_code_ = atoi(status_line.c_str() + code_begin + 1);

    size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
    while (pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        if (end == std::string::npos) {
            end = head.size();
        }
        std::string line = head.substr(pos, end - pos);
        pos = end + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        response_headers_[ToLower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
    }

    std::string encoding = ToLower(GetResponseHeader("transfer-encoding"));
    chunked_ = encoding.find("chunked") != std::string::npos;
    std::string length = GetResponseHeader("content-length");
    if (!chunked_ && !length.empty()) {
        content_length_ = static_cast<size_t>(strtoull(length.c_str(), nullptr, 10));
    }

    if (status_code_ >= 300 && status_code_ < 400) {
        std::string location = GetResponseHeader("location");
        if (!location.empty()) {
            *redirect_to = location;
        }
    }
    return true;
}

bool HttpClient::ReadBody(const std::function<bool(const char* data, size_t len)>& sink) {
    char chunk[4096];
    if (!chunked_) {
        size_t remaining = content_length_;
        bool bounded = !GetResponseHeader("content-length").empty();
        if (!impl_->buffer.empty()) {
            size_t take = bounded ? std::min(remaining, impl_->buffer.size()) : impl_->buffer.size();
            if (!sink(impl_->buffer.data(), take)) {
                return false;
            }
            if (bounded) {
                remaining -= take;
            }
            impl_->buffer.clear();
        }
        while (!bounded || remaining > 0) {
            int n = impl_->socket.Read(chunk, sizeof(chunk));
            if (n == TlsSocket::kClosed) {
                // Without Content-Length, close is the end of the body.
                return !bounded || remaining == 0;
            }
            if (n < 0) {
                error_ = "read failed while receiving body";
                return false;
            }
            size_t take = bounded ? std::min(remaining, static_cast<size_t>(n))
                                  : static_cast<size_t>(n);
            if (!sink(chunk, take)) {
                return false;
            }
            if (bounded) {
                remaining -= take;
            }
        }
        return true;
    }

    // Chunked transfer encoding.
    while (true) {
        size_t line_end;
        while ((line_end = impl_->buffer.find("\r\n")) == std::string::npos) {
            int n = impl_->socket.Read(chunk, sizeof(chunk));
            if (n <= 0) {
                error_ = "connection closed inside a chunk header";
                return false;
            }
            impl_->buffer.append(chunk, static_cast<size_t>(n));
        }
        std::string size_line = impl_->buffer.substr(0, line_end);
        impl_->buffer.erase(0, line_end + 2);
        size_t size = static_cast<size_t>(strtoul(size_line.c_str(), nullptr, 16));
        if (size == 0) {
            return true;  // trailer ignored
        }
        while (impl_->buffer.size() < size + 2) {
            int n = impl_->socket.Read(chunk, sizeof(chunk));
            if (n <= 0) {
                error_ = "connection closed inside a chunk body";
                return false;
            }
            impl_->buffer.append(chunk, static_cast<size_t>(n));
        }
        if (!sink(impl_->buffer.data(), size)) {
            return false;
        }
        impl_->buffer.erase(0, size + 2);
    }
}

std::string HttpClient::ReadAll(size_t max_bytes) {
    std::string body;
    bool overflow = false;
    ReadBody([&](const char* data, size_t len) {
        if (body.size() + len > max_bytes) {
            overflow = true;
            return false;
        }
        body.append(data, len);
        return true;
    });
    if (overflow) {
        error_ = "response body exceeded " + std::to_string(max_bytes) + " bytes";
        C1XZ_LOGE(TAG, "%s", error_.c_str());
        return {};
    }
    return body;
}

void HttpClient::Close() {
    if (impl_) {
        impl_->socket.Close();
    }
}

}  // namespace c1xz
