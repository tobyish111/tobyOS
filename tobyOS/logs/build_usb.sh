#!/bin/bash
# Build the USB/real-hardware ISO: the ORDINARY desktop, with the full
# Chromium payload staged so the taskbar pin is live (slice 119).
#
# Deliberately NOT a harness flavour:
#   - no CHROMIUM_BOOT / TKAPP_BOOT / TKAPP_CHROMEWIN, so nothing
#     auto-launches and you get the real login + desktop. A flavoured
#     build auto-logs-in as root, which is exactly the config slice 120
#     showed is blind to uid-dependent bugs.
#   - no SND_TRACE, so the audio layer does not narrate every ioctl to
#     the serial console.
#
# CHROME_FULL=1 stages programs/chromium/chrome-linux64 (the real browser)
# rather than chrome-headless-shell. The taskbar pin is payload-gated on
# /opt/chrome, so this is what makes it appear.
#
# Output: tobyOS.iso  -- write to USB with Rufus in DD mode.
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

# Flavour switch: `make` keys off mtime, NOT -D flags, so objects built
# with the chrome-harness defines would be silently reused. Drop them all.
echo "=== dropping objects from the previous flavour ==="
rm -f src/*.o
rm -f programs/chromewin/chromewin.o programs/chromewin/chromewin.elf

mkdir -p /c/t
# PROG_EXTRA_CFLAGS IS NOT OPTIONAL HERE. CHROME_FULL=1 only changes what the
# INITRD STAGES (/opt/chrome = the full browser, whose binary is `chrome`).
# chromewin picks its exec path at COMPILE time via #ifdef CHROME_FULL, and
# EXTRA_CFLAGS never reaches user programs -- so without it the staged browser
# is the full one while chromewin still execs the headless shell that is no
# longer there. It fails as `execve ... -1` / exit 127, which reads like a
# loader bug. The Makefile documents this exact trap at the
# LIBTOBY_KABI_PROGRAM_RULES definition; this script did not honour it, and a
# REAL-HARDWARE run is what surfaced it.
echo "=== building (plain desktop + CHROME_FULL) ==="
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          CHROME_FULL=1 PROG_EXTRA_CFLAGS=-DCHROME_FULL iso \
     >logs/build_usb.log 2>&1; then
    echo "BUILD FAIL -- tail:"; tail -30 logs/build_usb.log; exit 1
fi

[ -f tobyOS.iso ] || { echo "BUILD FAIL (no ISO)"; exit 1; }
if [ tobyOS.iso -ot tobyos.bin ]; then
    echo "BUILD FAIL: tobyOS.iso is OLDER than tobyos.bin -- stale image"
    exit 1
fi

echo
echo "=== flavour checks (these are the traps, so verify them) ==="
# Must NOT carry the auto-launch harness.
if grep -qa "TKAPP. launching" tobyos.bin; then
    echo "  WARNING: kernel still carries the TKAPP auto-launch harness"
else
    echo "  ok: no TKAPP auto-launch (you get the normal desktop)"
fi
# Must carry the FULL browser, not headless-shell.
if grep -a "initrd\] chromium" logs/build_usb.log | grep -q "FULL chrome"; then
    echo "  ok: FULL chrome staged under /opt/chrome (taskbar pin live)"
else
    echo "  WARNING: full chrome NOT staged -- taskbar pin will be absent:"
    grep -a "initrd\] chromium" logs/build_usb.log | head -2
fi
# THE BINARY IS THE GATE, not the make invocation: assert chromewin actually
# execs the browser that got staged. This check costs nothing and is the
# difference between finding the mismatch here and finding it on a USB stick.
if grep -qa '/opt/chrome/chrome-headless-shell' programs/chromewin/chromewin.elf; then
    echo "  FAIL: chromewin still execs chrome-headless-shell, which CHROME_FULL"
    echo "        does NOT stage -- it will exit 127. PROG_EXTRA_CFLAGS did not"
    echo "        reach it."
    exit 1
fi
if grep -qa '\-\-ozone-platform=x11' programs/chromewin/chromewin.elf; then
    echo "  ok: chromewin built with -DCHROME_FULL (execs /opt/chrome/chrome)"
else
    echo "  FAIL: chromewin lacks the CHROME_FULL argv -- wrong flavour"
    exit 1
fi

# Audio pieces present?
for f in lib/libasound.so.2 etc/asound.conf usr/share/alsa/alsa.conf \
         bin/audioplay bin/linux-sndtest; do
    if tar tf build/initrd.tar 2>/dev/null | grep -qx "$f"; then
        echo "  ok: $f"
    else
        echo "  MISSING: $f"
    fi
done

echo
echo "=== image ==="
ls -la tobyOS.iso
echo
echo "Write to USB with Rufus in DD mode (see the EliteDesk notes)."
