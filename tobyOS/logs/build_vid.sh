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
sleep 1   # slice 61f: give Windows a beat to release the ISO file lock
rm -f build/initrd.tar build/base.iso tobyOS.iso
# Slice 73: chromewin.o does NOT depend on the flavour define, so make keeps
# a stale object when only the KERNEL changed -- a CHROME_FULL initrd then
# ships a chromewin that execs the (absent) headless-shell path and dies
# with exit 127. Force it every flavour build.
rm -f programs/chromewin/chromewin.o programs/chromewin/chromewin.elf
# Slice 61f: FAIL LOUDLY. A locked tobyOS.iso (lingering QEMU) once made this
# make fail silently; the stale ISO then ran a whole 3-run batch in the WRONG
# FLAVOR (stock kernel, no chromewin) and the tail-2'd output hid the error.
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
     iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT -DTKAPP_BOOT -DTKAPP_CHROMEWIN"; then
    echo "BUILD FAILED (build_vid)"; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAILED (no ISO)"; exit 1; }
echo "=== iso mtime ==="
ls -l --time-style=full-iso tobyOS.iso
