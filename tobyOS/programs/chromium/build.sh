#!/usr/bin/env bash
# build.sh -- fetch a prebuilt, BSD-licensed Chromium "headless shell" for the
# tobyOS Chromium bring-up (docs/chromium-bringup-m0.md).
#
# This is OPT-IN and NOT committed: the payload is ~262 MB and the .gitignore
# excludes chrome-headless-shell-linux64/. Run this once; the Makefile then
# stages the tree under /opt/chrome in the initrd when CHROMIUM_BOOT-flavoured
# ISOs are built (see logs/chromium-m0.sh).
#
# chrome-headless-shell is the standalone "old headless" Chromium binary from
# Google's Chrome-for-Testing channel (open upstream, redistributable). It
# bundles SwiftShader (software GL/Vulkan) so no GPU driver is needed.
set -euo pipefail
cd "$(dirname "$0")"

CHANNEL="${CHANNEL:-Stable}"
JSON="https://googlechromelabs.github.io/chrome-for-testing/last-known-good-versions-with-downloads.json"

URL="$(curl -s -m 60 "$JSON" | python -c "
import sys,json
d=json.load(sys.stdin)['channels']['$CHANNEL']
print(d['version'])
for x in d['downloads']['chrome-headless-shell']:
    if x['platform']=='linux64': print(x['url'])
")"
VER="$(echo "$URL" | head -1)"
DL="$(echo "$URL" | tail -1)"
echo "[chromium] Chrome-for-Testing $CHANNEL = $VER"
echo "[chromium] $DL"

if [ ! -f chrome-headless-shell-linux64.zip ]; then
    curl -# -m 900 -o chrome-headless-shell-linux64.zip "$DL"
fi
rm -rf chrome-headless-shell-linux64
unzip -q chrome-headless-shell-linux64.zip
echo "[chromium] extracted:"
du -sh chrome-headless-shell-linux64
ls chrome-headless-shell-linux64/chrome-headless-shell
echo "[chromium] done. Next: bash logs/chromium-m0.sh"
