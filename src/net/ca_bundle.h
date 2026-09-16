#ifndef C1XZ_NET_CA_BUNDLE_H
#define C1XZ_NET_CA_BUNDLE_H

#include <cstddef>

namespace c1xz {

// Trusted root certificates in PEM form, compiled into the binary.
//
// The device's own rootfs has no usable trust store, and the factory firmware's
// HTTPS client does not verify certificates at all (that hole is what the root
// ADB procedure exploits). We therefore carry our own roots rather than trusting
// anything on the device.
//
// The bundle is generated at build time from third_party/cacert.pem. Point the
// C1XZ_CA_BUNDLE CMake variable at a trimmed PEM file to shrink it.
// Returns a NUL terminated buffer; *length excludes the terminator.
const char* CaBundlePem(size_t* length);

}  // namespace c1xz

#endif  // C1XZ_NET_CA_BUNDLE_H
