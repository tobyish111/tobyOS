#!/bin/bash
# Slice 49: fast rebuild for the video-pipeline probe. Only chromewin (main.c)
# and the staged /opt/chrome/vidtest.html changed -- NO kernel source touched,
# so kernel .o are reused (defines identical to build39.sh). Re-stages the
# initrd (picks up new /opt/chrome files + rebuilt chromewin) and the iso.
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t
export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
cd /c/CustomOS/tobyOS || exit 1
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1
rm -f build/initrd.tar build/base.iso tobyOS.iso
make "CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
     iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT -DTKAPP_BOOT -DTKAPP_CHROMEWIN"
echo "=== iso mtime ==="
ls -l --time-style=full-iso tobyOS.iso
