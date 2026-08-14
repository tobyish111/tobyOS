#!/bin/bash
# cwnet.sh -- does the DESKTOP browser load a REMOTE page? The gate for the
# socket-errno fix, run in the configuration the user actually has.
#
# WHY NOT tlsdiag.sh: that harness answers "what is chrome's exact SSL error"
# and it answered it (adapter.cc:157, net_error -17, from read() returning
# ENAMETOOLONG). But it drives chrome with --screenshot, which forces the
# X/SwiftShader RENDER TIER -- a separate front that crashes there for its own
# reasons, before a netlog is ever written. That configuration cannot report on
# a networking fix.
#
# chromewin + chrome-headless-shell + CDP is the path the taskbar pin uses, the
# path that rendered 169 frames on the EliteDesk, and the path that produced the
# actual bug report ("[net] FAILED #1: net::ERR_FILE_PATH_TOO_LONG"). So point
# chromewin at a REMOTE https URL and look for that exact symptom.
#
# VERDICT is deliberately two-sided: a page that loads must ALSO show no
# [net] FAILED, because chromewin renders an error page perfectly happily and
# "frames were produced" would otherwise read as success.
#
#   PASS  -> tobyprobe reports the real page AND zero [net] FAILED
#   FAIL  -> any [net] FAILED (the reported symptom, still present)
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t; export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
PY=/c/Users/tdude/AppData/Local/Programs/Python/Python311/python

URL="${URL:-https://example.com/}"
TAG="${TAG:-cwnet}"
export RUNSECS="${RUNSECS:-150}"
export SMP="${SMP:-4}"

# The quotes must survive THIS shell and make's recipe shell to reach clang.
# Single-quoting the backslash-quote is the only form that does (gpuperf.sh
# paid for this lesson; "-DCW_URL=\"$URL\"" collapses and clang then sees a
# bare identifier).
PROGF='-DCW_URL=\"'"$URL"'\"'
KCF="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT -DTKAPP_BOOT -DTKAPP_CHROMEWIN"

echo "=== [1/3] build: desktop + chromewin -> $URL ==="
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1; sleep 1
# Flavour switch: make keys off mtime, not -D flags. chromewin.o in particular
# does NOT depend on CW_URL as far as make can tell, so a stale object would
# quietly navigate to the PREVIOUS url and invert this experiment.
rm -f src/*.o
rm -f programs/chromewin/chromewin.o programs/chromewin/chromewin.elf
rm -f build/initrd.tar build/base.iso tobyOS.iso
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
     EXTRA_CFLAGS="$KCF" PROG_EXTRA_CFLAGS="$PROGF" \
     > "logs/${TAG}.build.log" 2>&1; then
    echo "BUILD FAILED -- tail:"; tail -25 "logs/${TAG}.build.log"; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAILED (no ISO)"; exit 1; }

# The URL is compiled INTO chromewin, so assert it is really in the binary.
# Without this, a silently-stale chromewin.elf reads as a clean pass.
if grep -qa -- "$URL" programs/chromewin/chromewin.elf; then
    echo "  ok: chromewin.elf carries $URL"
else
    echo "  FAIL: $URL is NOT in chromewin.elf -- stale object, verdict would be meaningless"
    exit 1
fi

echo "=== [2/3] run (whpx, SMP=$SMP, ${RUNSECS}s, /data fresh via snapshot=on) ==="
$PY logs/run_watch.py 2>&1 | tail -4
L="logs/${TAG}.log"
cp logs/run_watch.log "$L"

echo
echo "=== [3/3] VERDICT ==="
echo "--- the reported symptom (must be EMPTY) ---"
grep -a "\[net\] FAILED" "$L" | head -5
echo "--- what chrome says about the page it loaded ---"
grep -ao "tobyprobe rs=[a-z]* title=[^ ]* blen=[0-9]*" "$L" | tail -4
grep -ao "cwping\] rt=[0-9]*ms u=[^ ]*" "$L" | tail -3
echo "--- frames ---"
grep -ao "frame [0-9]*: [0-9]*x[0-9]* jpeg=[0-9]*" "$L" | tail -2
echo
nfail=$(grep -ac "\[net\] FAILED" "$L")
if [ "$nfail" != "0" ]; then
    echo "VERDICT: FAIL -- $nfail [net] FAILED line(s); the symptom is still present"
elif grep -aq "tobyprobe rs=complete" "$L" && grep -aq "blen=[1-9]" "$L"; then
    echo "VERDICT: PASS -- remote page loaded, no [net] FAILED"
else
    echo "VERDICT: INCONCLUSIVE -- no [net] FAILED, but no loaded-page evidence either"
fi
echo "=== log: $L ==="
