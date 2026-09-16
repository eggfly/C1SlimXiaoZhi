#!/usr/bin/env bash
# Push the build to a C1 Slim over root ADB and run it.
#
# Everything is written under /storage/c1/xiaozhi. Nothing touches the read-only
# root filesystem, the launcher, or any system startup script, so removing the
# directory undoes the whole install.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"

DEST=${C1XZ_DEST:-/storage/c1/xiaozhi}
BIN=build/device/c1xiaozhi
FONT=build/device/font.bin
ADB=${ADB:-adb}

if [ ! -f "$BIN" ]; then
    echo "$BIN not found; run tools/build.sh first" >&2
    exit 1
fi

if ! $ADB shell true >/dev/null 2>&1; then
    echo "no device over adb; see the root ADB guide in C1-Slim-Ports" >&2
    exit 1
fi

echo "== target"
$ADB shell 'cat /proc/device-tree/model 2>/dev/null; echo; uname -srm; id'

echo "== installing to $DEST"
$ADB shell "mkdir -p $DEST $DEST/sounds"
$ADB push "$BIN" "$DEST/c1xiaozhi.new" >/dev/null
# Replace atomically so a half-pushed binary is never executable.
$ADB shell "chmod 755 $DEST/c1xiaozhi.new && mv $DEST/c1xiaozhi.new $DEST/c1xiaozhi"
if [ -f "$FONT" ]; then
    $ADB push "$FONT" "$DEST/font.bin" >/dev/null
fi

# Prompt sounds are optional and are not redistributed here. Copy them from a
# checkout of the upstream firmware if one is next to this repository.
UPSTREAM=${C1XZ_UPSTREAM:-../xiaozhi-esp32}
if [ -d "$UPSTREAM/main/assets" ]; then
    echo "== copying prompt sounds from $UPSTREAM"
    for name in success exclamation popup low_battery vibration; do
        file="$UPSTREAM/main/assets/$name.ogg"
        [ -f "$file" ] && $ADB push "$file" "$DEST/sounds/$name.ogg" >/dev/null || true
    done
fi

echo "== installed"
$ADB shell "ls -l $DEST"

cat <<EOF

Run it on the device with:

  adb shell '$DEST/c1xiaozhi --log $DEST/run.log'

It takes over the screen and keyboard, pauses the factory UI, and restores both
when you hold Home for two seconds or send it SIGTERM.

First run needs the device to be on Wi-Fi already. It will show an activation
code to enter at xiaozhi.me.
EOF
