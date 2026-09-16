#include "net/http_client.h"
#include "test_framework.h"

using c1xz::Url;

TEST(ParsesHttpsWithDefaultPort) {
    Url url;
    CHECK(Url::Parse("https://api.tenclass.net/xiaozhi/ota/", &url));
    CHECK_STREQ(url.scheme, "https");
    CHECK_STREQ(url.host, "api.tenclass.net");
    CHECK_EQ(url.port, 443);
    CHECK_STREQ(url.path, "/xiaozhi/ota/");
    CHECK(url.secure);
}

TEST(ParsesExplicitPort) {
    Url url;
    CHECK(Url::Parse("ws://192.168.1.10:8000/xiaozhi/v1/", &url));
    CHECK_STREQ(url.host, "192.168.1.10");
    CHECK_EQ(url.port, 8000);
    CHECK_STREQ(url.path, "/xiaozhi/v1/");
    CHECK(!url.secure);
}

TEST(WssIsSecureAndDefaultsTo443) {
    Url url;
    CHECK(Url::Parse("wss://example.com/ws", &url));
    CHECK_EQ(url.port, 443);
    CHECK(url.secure);
}

TEST(EmptyPathBecomesSlash) {
    Url url;
    CHECK(Url::Parse("http://example.com", &url));
    CHECK_STREQ(url.path, "/");
    CHECK_EQ(url.port, 80);
}

TEST(ParsesIpv6Literal) {
    Url url;
    CHECK(Url::Parse("https://[2001:db8::1]:8443/path", &url));
    CHECK_STREQ(url.host, "2001:db8::1");
    CHECK_EQ(url.port, 8443);
    CHECK_STREQ(url.path, "/path");
}

TEST(StripsUserInfo) {
    // Credentials in the URL are dropped rather than sent.
    Url url;
    CHECK(Url::Parse("https://user:secret@example.com/x", &url));
    CHECK_STREQ(url.host, "example.com");
}

TEST(RejectsGarbage) {
    Url url;
    CHECK(!Url::Parse("not a url", &url));
    CHECK(!Url::Parse("ftp://example.com/", &url));
    CHECK(!Url::Parse("https://", &url));
    CHECK(!Url::Parse("https://example.com:0/", &url));
    CHECK(!Url::Parse("https://example.com:99999/", &url));
}

TEST(OriginOmitsPath) {
    Url url;
    CHECK(Url::Parse("https://example.com:8443/a/b?c=d", &url));
    CHECK_STREQ(url.Origin(), "https://example.com:8443");
    CHECK_STREQ(url.path, "/a/b?c=d");
}
