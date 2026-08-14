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
# ---------------------------------------------------------------------------
# NO CHROME_FULL. THIS IS THE POINT OF THE FIX, so read before changing it.
#
# There are two chrome configurations and only ONE of them renders:
#
#   headless-shell + CDP -> TobyTK window   (chromewin's default path)
#       PROVEN END TO END: multi-process, https, VP9 on a real YouTube watch
#       page, live resize, ~52fps via the viz shared-bitmap path.
#
#   full chrome + Ozone/X11                 (#ifdef CHROME_FULL)
#       "Route B", and it is REFUTED. Chrome takes the X11/GLX path and
#       CHECK-crashes (INT3) just after our stub X server's setup reply;
#       the arc notes record it as "bootstrap OK reached, 0 FRAMES".
#
# This script used to pass CHROME_FULL=1 -- a leftover from that arc -- which
# staged the full browser at /opt/chrome/chrome while chromewin, compiled
# WITHOUT -DCHROME_FULL (EXTRA_CFLAGS never reaches user programs), still
# exec'd /opt/chrome/chrome-headless-shell. That file is not staged in the
# CHROME_FULL branch, so it died `execve ... -1` / exit 127 on real hardware.
#
# Adding PROG_EXTRA_CFLAGS=-DCHROME_FULL would have made the two halves agree
# -- on the path that produces NO PIXELS. Dropping CHROME_FULL instead makes
# them agree on the path that works. The taskbar pin is gated on /opt/chrome
# EXISTING, and the headless-shell branch stages to exactly that path, so the
# pin is unaffected.
# ---------------------------------------------------------------------------
echo "=== building (plain desktop, headless-shell + CDP) ==="
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          iso \
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
# The taskbar pin is gated on the DIRECTORY /opt/chrome existing -- gui.c's
# taskbar_pin_count() does vfs_stat("/opt/chrome"), not a check for any
# particular binary. Both staging branches create that directory, so the pin is
# live either way. The old check here asked "is it the FULL browser?" and
# warned that the pin would be absent when it was not, which was wrong in
# exactly the configuration that actually renders.
if tar tf build/initrd.tar 2>/dev/null | grep -q "^opt/chrome/"; then
    echo "  ok: /opt/chrome staged -> taskbar pin live (gui.c gates on the dir)"
else
    echo "  FAIL: nothing staged under /opt/chrome -- the pin will be absent"
    exit 1
fi
# THE TWO HALVES MUST AGREE, and nobody was checking that -- which is the whole
# reason a USB stick shipped with a browser that exited 127. One half is what
# chromewin EXECS (baked in at compile time); the other is what the initrd
# STAGES. Assert them against each other, not each against an assumption.
CW_EXEC=""
grep -qa '/opt/chrome/chrome-headless-shell' programs/chromewin/chromewin.elf \
    && CW_EXEC="/opt/chrome/chrome-headless-shell"
grep -qa -- '--ozone-platform=x11' programs/chromewin/chromewin.elf \
    && CW_EXEC="/opt/chrome/chrome"
case "$CW_EXEC" in
  /opt/chrome/chrome-headless-shell) ;;
  "") echo "  FAIL: cannot tell which browser chromewin execs"; exit 1 ;;
  *)  echo "  FAIL: chromewin is a CHROME_FULL build (execs $CW_EXEC)."
      echo "        That is the Ozone/X11 route, which reaches bootstrap and"
      echo "        renders ZERO frames. Build without CHROME_FULL."
      exit 1 ;;
esac
if tar tf build/initrd.tar 2>/dev/null | grep -qx "opt/chrome/chrome-headless-shell"; then
    echo "  ok: chromewin execs $CW_EXEC, and the initrd stages it"
else
    echo "  FAIL: chromewin execs $CW_EXEC but the initrd does NOT contain it"
    echo "        -- this is the exit-127 that shipped on a USB stick."
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
