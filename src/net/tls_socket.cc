#include "net/tls_socket.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <unistd.h>

#include "net/ca_bundle.h"
#include "platform/log.h"

#define TAG "TlsSocket"

namespace c1xz {
namespace {

std::once_flag g_ca_once;
mbedtls_x509_crt g_ca_chain;
bool g_ca_ok = false;

std::mutex g_rng_mutex;
mbedtls_entropy_context g_entropy;
mbedtls_ctr_drbg_context g_ctr_drbg;
bool g_rng_ready = false;

// mbedtls calls this from several threads once we share one DRBG.
int RngWrapper(void* ctx, unsigned char* out, size_t len) {
    (void)ctx;
    std::lock_guard<std::mutex> lock(g_rng_mutex);
    return mbedtls_ctr_drbg_random(&g_ctr_drbg, out, len);
}

void InitCrypto() {
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);
    static const char kPers[] = "c1xiaozhi";
    int rc = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func, &g_entropy,
                                   reinterpret_cast<const unsigned char*>(kPers),
                                   sizeof(kPers) - 1);
    g_rng_ready = rc == 0;
    if (!g_rng_ready) {
        C1XZ_LOGE(TAG, "ctr_drbg seed failed: -0x%04x", -rc);
    }

    mbedtls_x509_crt_init(&g_ca_chain);
    size_t pem_len = 0;
    const char* pem = CaBundlePem(&pem_len);
    if (pem != nullptr && pem_len > 0) {
        // mbedtls needs the terminating NUL counted for PEM input.
        rc = mbedtls_x509_crt_parse(&g_ca_chain, reinterpret_cast<const unsigned char*>(pem),
                                    pem_len + 1);
        if (rc < 0) {
            C1XZ_LOGE(TAG, "CA bundle parse failed: -0x%04x", -rc);
        } else {
            if (rc > 0) {
                C1XZ_LOGW(TAG, "%d CA certificates in the bundle were skipped", rc);
            }
            g_ca_ok = true;
        }
    } else {
        C1XZ_LOGW(TAG, "no CA bundle compiled in; TLS verification will fail");
    }
}

int PollFd(int fd, short events, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = events;
    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

}  // namespace

struct TlsSocket::Impl {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    bool tls = false;
    bool open = false;
    int io_timeout_ms = 20000;

    Impl() {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
    }
    ~Impl() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_net_free(&net);
    }
};

TlsSocket::TlsSocket() : impl_(new Impl()) {}

TlsSocket::~TlsSocket() { Close(); }

void TlsSocket::EnsureCaChainLoaded() { std::call_once(g_ca_once, InitCrypto); }

void TlsSocket::SetError(const std::string& what, int code) {
    char buf[128] = {0};
    if (code != 0) {
        mbedtls_strerror(code, buf, sizeof(buf));
        last_error_ = what + ": " + buf;
    } else {
        last_error_ = what;
    }
    C1XZ_LOGE(TAG, "%s", last_error_.c_str());
}

bool TlsSocket::Connect(const Options& options) {
    EnsureCaChainLoaded();
    Close();
    impl_.reset(new Impl());
    impl_->tls = options.use_tls;
    impl_->io_timeout_ms = options.io_timeout_ms;

    std::string port = std::to_string(options.port);
    int rc = mbedtls_net_connect(&impl_->net, options.host.c_str(), port.c_str(),
                                 MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        SetError("tcp connect to " + options.host + " failed", rc);
        return false;
    }

    // Keep the socket blocking; timeouts come from poll() in Read/Write. A
    // half-dead network otherwise wedges the audio pipeline forever.
    if (mbedtls_net_set_block(&impl_->net) != 0) {
        SetError("failed to set blocking mode", 0);
        return false;
    }

    if (!options.use_tls) {
        impl_->open = true;
        return true;
    }

    if (!g_rng_ready) {
        SetError("RNG is not seeded", 0);
        return false;
    }

    rc = mbedtls_ssl_config_defaults(&impl_->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        SetError("ssl_config_defaults", rc);
        return false;
    }

    if (options.verify_peer) {
        if (!g_ca_ok) {
            SetError("peer verification requested but no CA bundle is available", 0);
            return false;
        }
        mbedtls_ssl_conf_authmode(&impl_->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&impl_->conf, &g_ca_chain, nullptr);
    } else {
        mbedtls_ssl_conf_authmode(&impl_->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&impl_->conf, RngWrapper, nullptr);
    mbedtls_ssl_conf_read_timeout(&impl_->conf, static_cast<uint32_t>(options.io_timeout_ms));

    rc = mbedtls_ssl_setup(&impl_->ssl, &impl_->conf);
    if (rc != 0) {
        SetError("ssl_setup", rc);
        return false;
    }
    // SNI. Servers behind a shared front end return the wrong certificate
    // without it, which then fails verification for a confusing reason.
    rc = mbedtls_ssl_set_hostname(&impl_->ssl, options.host.c_str());
    if (rc != 0) {
        SetError("ssl_set_hostname", rc);
        return false;
    }
    mbedtls_ssl_set_bio(&impl_->ssl, &impl_->net, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);

    while ((rc = mbedtls_ssl_handshake(&impl_->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                uint32_t flags = mbedtls_ssl_get_verify_result(&impl_->ssl);
                char why[256] = {0};
                mbedtls_x509_crt_verify_info(why, sizeof(why), "  ", flags);
                C1XZ_LOGE(TAG, "certificate rejected for %s:\n%s", options.host.c_str(), why);
            }
            SetError("tls handshake with " + options.host + " failed", rc);
            return false;
        }
    }

    impl_->open = true;
    C1XZ_LOGI(TAG, "connected to %s:%d (%s)", options.host.c_str(), options.port,
              mbedtls_ssl_get_ciphersuite(&impl_->ssl));
    return true;
}

int TlsSocket::Read(void* buffer, size_t length) {
    if (!impl_->open) {
        return kError;
    }
    if (!impl_->tls) {
        if (PollFd(impl_->net.fd, POLLIN, impl_->io_timeout_ms) == 0) {
            return kTimeout;
        }
        ssize_t n = ::read(impl_->net.fd, buffer, length);
        if (n == 0) {
            return kClosed;
        }
        if (n < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? kTimeout : kError;
        }
        return static_cast<int>(n);
    }

    int rc = mbedtls_ssl_read(&impl_->ssl, static_cast<unsigned char*>(buffer), length);
    if (rc > 0) {
        return rc;
    }
    switch (rc) {
        case 0:
        case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
            return kClosed;
        case MBEDTLS_ERR_SSL_TIMEOUT:
        case MBEDTLS_ERR_SSL_WANT_READ:
        case MBEDTLS_ERR_SSL_WANT_WRITE:
            return kTimeout;
        default:
            SetError("ssl read", rc);
            return kError;
    }
}

int TlsSocket::Write(const void* buffer, size_t length) {
    if (!impl_->open) {
        return kError;
    }
    const unsigned char* p = static_cast<const unsigned char*>(buffer);
    size_t written = 0;
    while (written < length) {
        int rc;
        if (impl_->tls) {
            rc = mbedtls_ssl_write(&impl_->ssl, p + written, length - written);
        } else {
            ssize_t n = ::write(impl_->net.fd, p + written, length - written);
            rc = n < 0 ? (errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                                 : MBEDTLS_ERR_NET_SEND_FAILED)
                       : static_cast<int>(n);
        }
        if (rc > 0) {
            written += static_cast<size_t>(rc);
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (PollFd(impl_->net.fd, POLLOUT, impl_->io_timeout_ms) <= 0) {
                return kTimeout;
            }
            continue;
        }
        SetError("socket write", rc);
        return kError;
    }
    return static_cast<int>(written);
}

bool TlsSocket::HasPendingData() const {
    if (!impl_->open || !impl_->tls) {
        return false;
    }
    return mbedtls_ssl_get_bytes_avail(&impl_->ssl) > 0;
}

bool TlsSocket::WaitReadable(int timeout_ms) {
    if (!impl_->open) {
        return false;
    }
    if (HasPendingData()) {
        return true;
    }
    return PollFd(impl_->net.fd, POLLIN, timeout_ms) > 0;
}

bool TlsSocket::connected() const { return impl_ && impl_->open; }

void TlsSocket::Close() {
    if (!impl_ || !impl_->open) {
        return;
    }
    if (impl_->tls) {
        mbedtls_ssl_close_notify(&impl_->ssl);
    }
    mbedtls_net_free(&impl_->net);
    impl_->open = false;
}

}  // namespace c1xz
