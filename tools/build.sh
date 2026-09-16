#!/usr/bin/env bash
# Build c1xiaozhi for the C1 Slim, or for the build host.
#
#   tools/build.sh                 device build (default)
#   tools/build.sh host            native build plus the unit tests
#   tools/build.sh host test       native build, then run the tests
#   tools/build.sh clean           remove the build directories
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"

TARGET=${1:-device}
ACTION=${2:-}
VERSION=${C1XZ_VERSION:-0.1.0}

if [ "$TARGET" = "clean" ]; then
    rm -rf build/device build/host
    echo "cleaned"
    exit 0
fi

if [ ! -d third_party/opus ] || [ ! -f third_party/cacert.pem ]; then
    echo "== third_party is missing, fetching"
    ./tools/fetch-deps.sh
fi

if [ ! -f assets/font_ascii.inc ]; then
    echo "== generating the compiled-in ASCII font"
    python3 tools/build_font.py
fi

case "$TARGET" in
host)
    cmake -S . -B build/host -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DC1XZ_VERSION="$VERSION" >/dev/null
    ninja -C build/host
    echo
    echo "host binary:  build/host/c1xiaozhi"
    echo "host tests:   build/host/tests/c1xz_tests"
    if [ "$ACTION" = "test" ]; then
        echo
        ./build/host/tests/c1xz_tests
    fi
    ;;
device)
    command -v zig >/dev/null || { echo "zig is required; see docs/BUILDING.md" >&2; exit 1; }
    cmake -S . -B build/device -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=cmake/mipsel-zig.cmake \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DC1XZ_VERSION="$VERSION" >/dev/null
    ninja -C build/device

    # The font file is deployed next to the binary rather than compiled in;
    # the CJK set is about 900 KiB.
    python3 tools/build_font.py --bin-out build/device/font.bin >/dev/null

    BIN=build/device/c1xiaozhi
    echo
    echo "== $BIN"
    ls -l "$BIN" | awk '{printf "   size: %s bytes\n", $5}'

    # Verify the ELF is what the device can actually load. These attributes
    # match the factory mpenMain exactly; a mismatch means it will not run.
    if command -v llvm-readelf >/dev/null; then
        READELF=llvm-readelf
    elif [ -x /opt/homebrew/opt/llvm@21/bin/llvm-readelf ]; then
        READELF=/opt/homebrew/opt/llvm@21/bin/llvm-readelf
    elif command -v readelf >/dev/null; then
        READELF=readelf
    else
        READELF=""
    fi

    if [ -n "$READELF" ]; then
        "$READELF" -h "$BIN" | grep -E "Class|Data|Machine|Flags" | sed 's/^/   /'
        "$READELF" -A "$BIN" 2>/dev/null | grep -E "ISA:|GPR size|CPR1 size|FP ABI" | sed 's/^/   /'
        if "$READELF" -d "$BIN" 2>/dev/null | grep -q NEEDED; then
            echo "   ERROR: the binary is dynamically linked" >&2
            exit 1
        fi
        echo "   static: yes"
    else
        echo "   (no readelf available; skipping the ELF check)"
    fi
    echo
    echo "deploy with: tools/deploy.sh"
    ;;
*)
    echo "usage: $0 [device|host|clean] [test]" >&2
    exit 2
    ;;
esac
