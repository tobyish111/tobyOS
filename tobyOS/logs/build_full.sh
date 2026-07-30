#!/bin/bash
# Slice 69 (Route B milestone 1): build the CHROME_FULL flavour -- the FULL
# chrome binary (programs/chromium/chrome-linux64) staged at /opt/chrome
# instead of the headless-only shell, with -DCHROME_FULL so chromewin execs
# /opt/chrome/chrome --headless=new.
#
# Why: the zero-copy frame path (tier 2.5, measured worth ~2.3x in slice 68)
# needs chrome to composite into memory we own, which means an Ozone backend
# -- and chrome-headless-shell has none. Milestone 1 is simply "does the
# bigger binary + its 4 extra DSOs (cairo/cups/pango/smime3) load and run at
# all on the Linux ABI", still headless, before any X11 work starts.
#
# NOTE the initrd grows ~392MB -> ~570MB (RAM-loaded), so runs need plenty of
# guest memory; run_watch.py already uses -m 6144.
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t
export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
cd /c/CustomOS/tobyOS || exit 1
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 1
rm -f build/initrd.tar build/base.iso tobyOS.iso
if ! make CHROME_FULL=1 PROG_EXTRA_CFLAGS=-DCHROME_FULL "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
     "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
     iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT -DTKAPP_BOOT -DTKAPP_CHROMEWIN -DCHROME_FULL"; then
    echo "BUILD FAILED (build_full)"; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAILED (no ISO)"; exit 1; }
echo "=== iso mtime ==="
ls -l --time-style=full-iso tobyOS.iso
