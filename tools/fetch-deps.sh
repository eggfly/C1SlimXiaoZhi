#!/usr/bin/env bash
# Download and verify the pinned third-party sources into third_party/.
# Everything is fetched as a release tarball with a SHA-256 check, so a build is
# reproducible and does not depend on cloning large git histories.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
DEST="$HERE/third_party"
CACHE="${C1XZ_DL_CACHE:-$DEST/.cache}"
mkdir -p "$DEST" "$CACHE"

# name|version|url|sha256|top-level-dir-in-archive
DEPS=(
"opus|1.5.2|https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz|65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1|opus-1.5.2"
"mbedtls|3.6.7|https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2|a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6|mbedtls-3.6.7"
"cJSON|1.7.18|https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz|3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5|cJSON-1.7.18"
"tinyalsa|2.0.0|https://github.com/tinyalsa/tinyalsa/archive/refs/tags/v2.0.0.tar.gz|573ae0b2d3480851c1d2a12503ead2beea27f92d44ed47b74b553ba947994ef1|tinyalsa-2.0.0"
)

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}

for entry in "${DEPS[@]}"; do
    IFS='|' read -r name version url want top <<<"$entry"
    if [ -d "$DEST/$name" ] && [ -z "${C1XZ_FORCE_FETCH:-}" ]; then
        echo "== $name $version already present, skipping"
        continue
    fi
    archive="$CACHE/$(basename "$url")"
    if [ ! -f "$archive" ]; then
        echo "== fetching $name $version"
        curl -fsSL --retry 3 --retry-delay 2 -o "$archive.part" "$url"
        mv "$archive.part" "$archive"
    fi
    got=$(sha256_of "$archive")
    if [ "$want" = "SKIP" ]; then
        echo "   sha256 $got  ($name, not pinned yet - record it in this script)"
    elif [ "$got" != "$want" ]; then
        echo "!! sha256 mismatch for $name" >&2
        echo "   want $want" >&2
        echo "   got  $got" >&2
        exit 1
    fi
    echo "== extracting $name"
    rm -rf "$DEST/$name" "$DEST/.tmp-$name"
    mkdir -p "$DEST/.tmp-$name"
    tar -xf "$archive" -C "$DEST/.tmp-$name"
    mv "$DEST/.tmp-$name/$top" "$DEST/$name"
    rmdir "$DEST/.tmp-$name"
done

# GNU Unifont, as a .hex bitmap dump. The factory firmware ships the same font
# family, it covers ASCII and CJK at 8x16 / 16x16, and its licence (GPLv2+ with
# the font embedding exception) allows embedding the glyphs in a binary.
if [ ! -f "$DEST/unifont.hex" ] || [ -n "${C1XZ_FORCE_FETCH:-}" ]; then
    echo "== fetching unifont"
    curl -fsSL --retry 3 --retry-delay 2 -o "$CACHE/unifont.hex.gz" \
        https://unifoundry.com/pub/unifont/unifont-16.0.01/font-builds/unifont-16.0.01.hex.gz
    gunzip -c "$CACHE/unifont.hex.gz" > "$DEST/unifont.hex"
    echo "   $(wc -l < "$DEST/unifont.hex" | tr -d ' ') glyphs"
fi

# Trusted roots. The device has no usable trust store of its own, so we compile
# our own bundle in. Not pinned by hash on purpose: it is expected to change as
# CAs rotate, and a stale bundle eventually breaks TLS to xiaozhi.me.
if [ ! -f "$DEST/cacert.pem" ] || [ -n "${C1XZ_FORCE_FETCH:-}" ]; then
    echo "== fetching cacert.pem (Mozilla CA bundle via curl.se)"
    curl -fsSL --retry 3 --retry-delay 2 -o "$DEST/cacert.pem.part" \
        https://curl.se/ca/cacert.pem
    mv "$DEST/cacert.pem.part" "$DEST/cacert.pem"
    echo "   $(grep -c 'BEGIN CERTIFICATE' "$DEST/cacert.pem") certificates, sha256 $(sha256_of "$DEST/cacert.pem")"
fi

echo
echo "third_party is ready:"
for entry in "${DEPS[@]}"; do
    IFS='|' read -r name version _ _ _ <<<"$entry"
    printf '  %-10s %s\n' "$name" "$version"
done
