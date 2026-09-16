#include "net/websocket_client.h"

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include <cstring>

#include "net/http_client.h"
#include "net/tls_socket.h"
#include "platform/event_loop.h"
#include "platform/log.h"

#define TAG "WebSocket"

namespace c1xz {
namespace {

constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xa;

// RFC 6455 section 1.3.
constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// A single frame larger than this is treated as protocol abuse. Opus frames are
// a few hundred bytes and JSON messages a few kilobytes, so this is generous.
constexpr size_t kMaxFrameBytes = 1024 * 1024;

std::string Base64(const unsigned char* data, size_t len) {
    size_t needed = 0;
    mbedtls_base64_encode(nullptr, 0, &needed, data, len);
    std::string out(needed, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), out.size(), &written, data,
                              len) != 0) {
        return {};
    }
    out.resize(written);
    return out;
}

std::string ToLower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

}  // namespace

struct WebSocketClient::Impl {
    TlsSocket socket;
    std::string rx;  // bytes read but not yet consumed as a frame
    // Fragmented message reassembly.
    std::vector<char> message;
    uint8_t message_opcode = 0;
    int64_t last_rx_ms = 0;
    int64_t last_tx_ms = 0;
};

WebSocketClient::WebSocketClient() : impl_(new Impl()) {}

WebSocketClient::~WebSocketClient() { Close(); }

void WebSocketClient::SetHeader(const std::string& key, const std::string& value) {
    headers_[key] = value;
}
void WebSocketClient::SetTimeout(int milliseconds) { timeout_ms_ = milliseconds; }
void WebSocketClient::SetVerifyPeer(bool verify) { verify_peer_ = verify; }
void WebSocketClient::SetPingInterval(int milliseconds) { ping_interval_ms_ = milliseconds; }
void WebSocketClient::OnData(DataHandler handler) { on_data_ = std::move(handler); }
void WebSocketClient::OnConnected(VoidHandler handler) { on_connected_ = std::move(handler); }
void WebSocketClient::OnDisconnected(VoidHandler handler) { on_disconnected_ = std::move(handler); }
void WebSocketClient::OnError(ErrorHandler handler) { on_error_ = std::move(handler); }

bool WebSocketClient::IsConnected() const { return connected_.load(); }

void WebSocketClient::Fail(const std::string& message) {
    error_ = message;
    C1XZ_LOGE(TAG, "%s", message.c_str());
    if (on_error_) {
        on_error_(message);
    }
}

bool WebSocketClient::Handshake(const std::string& url) {
    Url parsed;
    if (!Url::Parse(url, &parsed)) {
        Fail("malformed websocket URL: " + url);
        return false;
    }

    TlsSocket::Options options;
    options.host = parsed.host;
    options.port = parsed.port;
    options.use_tls = parsed.secure;
    options.verify_peer = verify_peer_;
    options.connect_timeout_ms = timeout_ms_;
    // The receive loop polls, so the socket timeout only bounds the handshake
    // and any single blocking read inside a frame.
    options.io_timeout_ms = timeout_ms_;
    if (!impl_->socket.Connect(options)) {
        Fail(impl_->socket.last_error());
        return false;
    }

    unsigned char nonce[16];
    for (size_t i = 0; i < sizeof(nonce); ++i) {
        nonce[i] = static_cast<unsigned char>(rand() & 0xff);
    }
    std::string key = Base64(nonce, sizeof(nonce));

    std::string request = "GET " + parsed.path + " HTTP/1.1\r\n";
    request += "Host: " + parsed.host;
    if ((parsed.secure && parsed.port != 443) || (!parsed.secure && parsed.port != 80)) {
        request += ":" + std::to_string(parsed.port);
    }
    request += "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + key + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    for (const auto& header : headers_) {
        request += header.first + ": " + header.second + "\r\n";
    }
    request += "\r\n";

    if (impl_->socket.Write(request.data(), request.size()) < 0) {
        Fail("failed to send websocket handshake");
        return false;
    }

    char chunk[1024];
    size_t header_end;
    while ((header_end = impl_->rx.find("\r\n\r\n")) == std::string::npos) {
        if (impl_->rx.size() > 32 * 1024) {
            Fail("websocket handshake response too large");
            return false;
        }
        int n = impl_->socket.Read(chunk, sizeof(chunk));
        if (n <= 0) {
            Fail("connection closed during websocket handshake");
            return false;
        }
        impl_->rx.append(chunk, static_cast<size_t>(n));
    }
    std::string head = impl_->rx.substr(0, header_end);
    impl_->rx.erase(0, header_end + 4);

    if (head.compare(0, 9, "HTTP/1.1 ") != 0 || head.compare(9, 3, "101") != 0) {
        size_t line_end = head.find("\r\n");
        Fail("websocket upgrade refused: " + head.substr(0, line_end));
        return false;
    }

    // Verify Sec-WebSocket-Accept. A proxy that echoes 101 without the right
    // digest is not speaking WebSocket to us.
    std::string expect_source = key + kGuid;
    unsigned char digest[20];
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(expect_source.data()),
                 expect_source.size(), digest);
    std::string expected = ToLower(Base64(digest, sizeof(digest)));

    std::string lower_head = ToLower(head);
    size_t accept_pos = lower_head.find("sec-websocket-accept:");
    if (accept_pos == std::string::npos) {
        Fail("server did not send Sec-WebSocket-Accept");
        return false;
    }
    size_t value_begin = lower_head.find_first_not_of(" \t", accept_pos + 21);
    size_t value_end = lower_head.find("\r\n", value_begin);
    std::string got = lower_head.substr(value_begin, value_end - value_begin);
    while (!got.empty() && (got.back() == ' ' || got.back() == '\t')) {
        got.pop_back();
    }
    if (got != expected) {
        Fail("Sec-WebSocket-Accept mismatch");
        return false;
    }

    impl_->last_rx_ms = MonotonicMs();
    impl_->last_tx_ms = impl_->last_rx_ms;
    return true;
}

bool WebSocketClient::Connect(const std::string& url) {
    Close();
    impl_.reset(new Impl());
    error_.clear();
    stopping_.store(false);

    if (!Handshake(url)) {
        return false;
    }

    connected_.store(true);
    receive_thread_ = std::thread([this] { ReceiveLoop(); });
    if (on_connected_) {
        on_connected_();
    }
    return true;
}

bool WebSocketClient::SendFrame(uint8_t opcode, const void* payload, size_t length) {
    if (!connected_.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(send_mutex_);

    unsigned char header[14];
    size_t header_len = 0;
    header[header_len++] = static_cast<unsigned char>(0x80 | opcode);  // FIN set

    // A client must mask every frame it sends (RFC 6455 section 5.3).
    unsigned char mask[4];
    for (size_t i = 0; i < sizeof(mask); ++i) {
        mask[i] = static_cast<unsigned char>(rand() & 0xff);
    }

    if (length < 126) {
        header[header_len++] = static_cast<unsigned char>(0x80 | length);
    } else if (length <= 0xffff) {
        header[header_len++] = 0x80 | 126;
        header[header_len++] = static_cast<unsigned char>((length >> 8) & 0xff);
        header[header_len++] = static_cast<unsigned char>(length & 0xff);
    } else {
        header[header_len++] = 0x80 | 127;
        for (int shift = 56; shift >= 0; shift -= 8) {
            header[header_len++] = static_cast<unsigned char>((static_cast<uint64_t>(length) >>
                                                               shift) & 0xff);
        }
    }
    memcpy(header + header_len, mask, sizeof(mask));
    header_len += sizeof(mask);

    if (impl_->socket.Write(header, header_len) < 0) {
        return false;
    }
    if (length > 0) {
        // Mask in bounded blocks so a large frame does not need a second full
        // copy of the payload.
        const unsigned char* src = static_cast<const unsigned char*>(payload);
        unsigned char block[1024];
        size_t sent = 0;
        while (sent < length) {
            size_t take = std::min(sizeof(block), length - sent);
            for (size_t i = 0; i < take; ++i) {
                block[i] = src[sent + i] ^ mask[(sent + i) & 3];
            }
            if (impl_->socket.Write(block, take) < 0) {
                return false;
            }
            sent += take;
        }
    }
    impl_->last_tx_ms = MonotonicMs();
    return true;
}

bool WebSocketClient::Send(const void* data, size_t length, bool binary) {
    return SendFrame(binary ? kOpBinary : kOpText, data, length);
}

bool WebSocketClient::SendText(const std::string& text) {
    return SendFrame(kOpText, text.data(), text.size());
}

void WebSocketClient::ReceiveLoop() {
    auto read_more = [this]() -> bool {
        char chunk[4096];
        int n = impl_->socket.Read(chunk, sizeof(chunk));
        if (n == TlsSocket::kTimeout) {
            return true;  // nothing to add, not an error
        }
        if (n <= 0) {
            return false;
        }
        impl_->rx.append(chunk, static_cast<size_t>(n));
        impl_->last_rx_ms = MonotonicMs();
        return true;
    };

    // Consumes one whole frame from impl_->rx if there is one.
    enum class Step { kNeedMore, kConsumed, kStop };
    auto step_once = [this]() -> Step {
        if (impl_->rx.size() < 2) {
            return Step::kNeedMore;
        }
        const unsigned char* p = reinterpret_cast<const unsigned char*>(impl_->rx.data());
        bool fin = (p[0] & 0x80) != 0;
        uint8_t opcode = p[0] & 0x0f;
        bool masked = (p[1] & 0x80) != 0;
        uint64_t length = p[1] & 0x7f;
        size_t offset = 2;

        if (length == 126) {
            if (impl_->rx.size() < offset + 2) return Step::kNeedMore;
            length = (static_cast<uint64_t>(p[2]) << 8) | p[3];
            offset += 2;
        } else if (length == 127) {
            if (impl_->rx.size() < offset + 8) return Step::kNeedMore;
            length = 0;
            for (int i = 0; i < 8; ++i) {
                length = (length << 8) | p[offset + i];
            }
            offset += 8;
        }
        if (length > kMaxFrameBytes) {
            Fail("frame of " + std::to_string(length) + " bytes exceeds the limit");
            return Step::kStop;
        }
        // A server must not mask (RFC 6455 section 5.1), but handle it anyway.
        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (impl_->rx.size() < offset + 4) return Step::kNeedMore;
            memcpy(mask, p + offset, 4);
            offset += 4;
        }
        if (impl_->rx.size() < offset + length) {
            return Step::kNeedMore;
        }

        std::vector<char> payload(static_cast<size_t>(length));
        if (length > 0) {
            memcpy(payload.data(), impl_->rx.data() + offset, static_cast<size_t>(length));
            if (masked) {
                for (size_t i = 0; i < payload.size(); ++i) {
                    payload[i] = static_cast<char>(payload[i] ^ mask[i & 3]);
                }
            }
        }
        impl_->rx.erase(0, offset + static_cast<size_t>(length));

        switch (opcode) {
            case kOpPing:
                SendFrame(kOpPong, payload.data(), payload.size());
                return Step::kConsumed;
            case kOpPong:
                return Step::kConsumed;
            case kOpClose:
                SendFrame(kOpClose, nullptr, 0);
                return Step::kStop;
            case kOpContinuation:
            case kOpText:
            case kOpBinary: {
                if (opcode != kOpContinuation) {
                    impl_->message.clear();
                    impl_->message_opcode = opcode;
                }
                impl_->message.insert(impl_->message.end(), payload.begin(), payload.end());
                if (impl_->message.size() > kMaxFrameBytes) {
                    Fail("fragmented message exceeded the limit");
                    return Step::kStop;
                }
                if (fin && on_data_) {
                    on_data_(impl_->message.data(), impl_->message.size(),
                             impl_->message_opcode == kOpBinary);
                    impl_->message.clear();
                }
                return Step::kConsumed;
            }
            default:
                C1XZ_LOGW(TAG, "ignoring unknown opcode 0x%x", opcode);
                return Step::kConsumed;
        }
    };

    while (!stopping_.load()) {
        bool progressed = false;
        bool stop = false;
        while (true) {
            Step step = step_once();
            if (step == Step::kStop) {
                stop = true;
                break;
            }
            if (step == Step::kNeedMore) {
                break;
            }
            progressed = true;
        }
        if (stop || stopping_.load()) {
            break;
        }

        if (ping_interval_ms_ > 0) {
            int64_t now = MonotonicMs();
            if (now - impl_->last_tx_ms > ping_interval_ms_ &&
                now - impl_->last_rx_ms > ping_interval_ms_) {
                SendFrame(kOpPing, nullptr, 0);
            }
        }

        if (!progressed) {
            if (!impl_->socket.WaitReadable(500)) {
                continue;  // idle; loop back to check ping and stop flags
            }
            if (!read_more()) {
                break;
            }
        } else if (impl_->socket.HasPendingData()) {
            if (!read_more()) {
                break;
            }
        }
    }

    connected_.store(false);
    impl_->socket.Close();
    if (on_disconnected_ && !stopping_.load()) {
        on_disconnected_();
    }
}

void WebSocketClient::Close() {
    if (!impl_) {
        return;
    }
    bool was_connected = connected_.exchange(false);
    stopping_.store(true);
    if (was_connected) {
        SendFrame(kOpClose, nullptr, 0);
    }
    impl_->socket.Close();
    if (receive_thread_.joinable()) {
        if (receive_thread_.get_id() == std::this_thread::get_id()) {
            receive_thread_.detach();
        } else {
            receive_thread_.join();
        }
    }
}

}  // namespace c1xz
