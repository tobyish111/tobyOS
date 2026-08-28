#!/bin/bash
# vnc.sh -- does a REAL VNC client get an authenticated, interactive desktop?
#
# The oracle is logs/vncprobe.py: an independent RFB 3.8 client that computes
# its DES authentication response with OPENSSL, not with src/des.c. A bug in
# our DES therefore cannot cancel against a matching bug in the test -- the
# two implementations have to agree or the handshake fails outright.
#
# Two halves, and both matter:
#   - the CLIENT half (vncprobe) proves the protocol, the auth, and that
#     complete framebuffer updates decode under Raw and Hextile;
#   - the GUEST half (serial log) proves remote input actually reached the
#     PS/2 driver path, which the client cannot observe from outside.
#
# A gate that only checked the client half would pass on a server that
# accepted KeyEvents and dropped them on the floor.
#
#   PASS -> vncprobe 9/9 AND the guest logged the injected key/pointer events
#   FAIL -> anything else
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t; export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
PY=/c/Users/tdude/AppData/Local/Programs/Python/Python311/python

TAG="${TAG:-vnc}"
export RUNSECS="${RUNSECS:-260}"
export HOSTFWD="hostfwd=tcp::5900-:5900"
export VNCPASS="${VNCPASS:-tobyvnc}"

echo "=== [1/4] build (-DVNC_BOOT seeds the gate password) ==="
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1; sleep 1

# EXTRA_CFLAGS is `?=` in the Makefile, so it must RESTATE the fast-boot
# defaults -- passing only -DVNC_BOOT drops FAST_BOOT/QUICK_BOOT and the
# guest takes so long to boot that the probe times out on a healthy build.
#
# make keys off mtime, not -D flags: without dropping these objects the
# VNC_BOOT flavour would silently reuse the plain ones and the server would
# never come up (no password file), which reads as "VNC is broken".
rm -f src/vnc.o src/net.o
rm -f build/initrd.tar build/base.iso tobyOS.iso

if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DVNC_BOOT" \
     > "logs/${TAG}.build.log" 2>&1; then
    echo "BUILD FAILED -- tail:"; tail -25 "logs/${TAG}.build.log"; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAILED (no ISO)"; exit 1; }

# Prove the flavour is really in the binary. A stale src/vnc.o would leave
# the harness inert and every later check would misreport why.
if grep -qa "VNC_BOOT: seeded gate password" tobyos.bin; then
    echo "  ok: VNC_BOOT harness is in tobyos.bin"
else
    echo "  FAIL: VNC_BOOT marker absent from tobyos.bin -- stale object"
    exit 1
fi
grep -qa "RFB 003.008" tobyos.bin \
    && echo "  ok: RFB server is in tobyos.bin" \
    || { echo "  FAIL: no RFB server in tobyos.bin"; exit 1; }

echo
echo "=== [2/4] boot (${RUNSECS}s, tcp/5900 forwarded to the guest) ==="
$PY logs/run_watch.py > "logs/${TAG}.run.log" 2>&1 &
RUNPID=$!

echo
echo "=== [3/4] probe with an independent RFB client ==="
# vncprobe waits for the listener itself; the guest needs ~2min to reach
# network bring-up under emulation.
$PY logs/vncprobe.py
PROBE_RC=$?

wait $RUNPID
L="logs/${TAG}.log"
cp logs/run_watch.log "$L" 2>/dev/null

echo
echo "=== [4/4] guest-side evidence ==="
listening=$(grep -ac "listening on TCP port 5900" "$L" 2>/dev/null || echo 0)
authed=$(grep -ac "\[vnc\] authenticated" "$L" 2>/dev/null || echo 0)
failed=$(grep -ac "authentication FAILED" "$L" 2>/dev/null || echo 0)
keys=$(grep -ac "\[vnc\] key sym=" "$L" 2>/dev/null || echo 0)
ptrs=$(grep -ac "\[vnc\] ptr (" "$L" 2>/dev/null || echo 0)

echo "  listening:            $listening"
echo "  authenticated:        $authed"
echo "  auth failures logged: $failed   (1 expected -- the wrong-password check)"
echo "  key events injected:  $keys"
echo "  ptr events injected:  $ptrs"
echo
echo "  --- injected keys, as the driver saw them ---"
grep -a "\[vnc\] key sym=" "$L" 2>/dev/null | head -8 | sed 's/^/    /'
grep -a "\[vnc\] ptr (" "$L" 2>/dev/null | head -3 | sed 's/^/    /'

# The guest half. A key that is ACCEPTED but never reaches the driver would
# leave these at zero while the client half still passed.
GUEST_OK=1
[ "$listening" -ge 1 ] || { echo "  FAIL: server never listened"; GUEST_OK=0; }
[ "$authed"    -ge 1 ] || { echo "  FAIL: no successful authentication"; GUEST_OK=0; }
[ "$failed"    -ge 1 ] || { echo "  FAIL: the wrong password was never rejected"; GUEST_OK=0; }
[ "$keys"      -ge 4 ] || { echo "  FAIL: remote keys did not reach the driver"; GUEST_OK=0; }
[ "$ptrs"      -ge 1 ] || { echo "  FAIL: remote pointer did not reach the driver"; GUEST_OK=0; }

echo
if [ "$PROBE_RC" -eq 0 ] && [ "$GUEST_OK" -eq 1 ]; then
    echo "VERDICT: PASS -- authenticated VNC session, pixels and input both flow"
    exit 0
fi
echo "VERDICT: FAIL (probe_rc=$PROBE_RC guest_ok=$GUEST_OK)"
echo "  serial log: $L"
exit 1
