#ifndef C1XZ_NET_TLS_SOCKET_H
#define C1XZ_NET_TLS_SOCKET_H

#include <cstddef>
#include <memory>
#include <string>

namespace c1xz {

// Blocking TCP socket with optional TLS, built on mbedtls.
//
// One instance is owned by one connection. It is not thread safe: the
// WebSocket and MQTT clients above it serialise their writes and keep reads on
// a single receive thread.
class TlsSocket {
public:
    struct Options {
        std::string host;
        int port = 443;
        bool use_tls = true;
        // Turning this off disables certificate and hostname checking. It
        // exists for bringing up a self-signed local server, and for the narrow
        // case of fetching the time before the clock is trustworthy.
        bool verify_peer = true;
        int connect_timeout_ms = 15000;
        int io_timeout_ms = 20000;
    };

    enum Result : int {
        kClosed = 0,
        kError = -1,
        kTimeout = -2,
    };

    TlsSocket();
    ~TlsSocket();

    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    bool Connect(const Options& options);

    // Returns the byte count, kClosed on a clean peer shutdown, kTimeout when
    // io_timeout_ms elapsed with no data, or kError.
    int Read(void* buffer, size_t length);

    // Writes everything or fails. Returns the byte count or a negative Result.
    int Write(const void* buffer, size_t length);

    void Close();

    bool connected() const;

    // True when TLS has already decrypted bytes sitting in its own buffer. A
    // poll() on the fd would miss those, so the receive loop has to ask.
    bool HasPendingData() const;

    // Waits for readability. Returns true if data may be available.
    bool WaitReadable(int timeout_ms);

    const std::string& last_error() const { return last_error_; }

    // Loads the trusted roots once per process. Called automatically.
    static void EnsureCaChainLoaded();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string last_error_;

    void SetError(const std::string& what, int code);
};

}  // namespace c1xz

#endif  // C1XZ_NET_TLS_SOCKET_H
