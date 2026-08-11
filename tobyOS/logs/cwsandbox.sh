#!/bin/bash
# Boot chromewin with the slice-14 privilege drop ENABLED (CW_SANDBOX), i.e.
# Chromium with its own sandbox instead of --no-sandbox.
#
#   bash logs/cwsandbox.sh <tag>      # tag names the log; NEVER reuse one
#
# Exit status is the result: 0 = chrome survived the sandbox bring-up AND
# actually bootstrapped, 1 = not.
#
# THAT SECOND CLAUSE WAS MISSING AND THE GATE LIED BECAUSE OF IT. The checks
# below started as "the drop happened, chromium did not refuse root, no
# credentials.cc CHECK, no clone was refused" -- all four of which can be true
# on a run where the browser never comes up at all. It reported GREEN on
# exactly such a run: every sandbox milestone passed, and then
# `[chromewin] bootstrap failed`. A gate whose success path does not include
# the thing it is gating is not a gate. The bootstrap check is now the LAST
# word, and it is the one that decides.
#
# --- the traps this script exists to avoid, all of them already paid for ---
#
# 1. -DCHROMIUM_BOOT is the WRONG launcher. It has its own in-kernel launcher
#    that execs /opt/chrome directly and never goes through chromewin, so none
#    of slice 14's code is on the path -- a whole boot testing nothing. The
#    chromewin path is -DTKAPP_BOOT -DTKAPP_CHROMEWIN.
# 2. EXTRA_CFLAGS does NOT reach user programs. chromewin needs
#    PROG_EXTRA_CFLAGS=-DCW_SANDBOX. This exact mistake once made -DCHROME_FULL
#    silently miss chromewin.
# 3. Neither flag hashes into the build, so nothing rebuilds on a flavour
#    switch. Delete the objects by hand.
# 4. Gate on the BINARIES, not on the build succeeding: the staged chromewin
#    must carry the drop and must NOT carry --no-sandbox, and the kernel must
#    reference /bin/chromewin. Asserting the OLD string is GONE is what
#    separates "rebuilt" from "the previous flavour happened to contain it".
# 5. A stopped runner does not kill its QEMU. Check before believing a run.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

TAG="${1:-cwsb}"
LOG="logs/cwsandbox_$TAG.log"
BUILDLOG="logs/cwsandbox_$TAG.build.log"
RUNSECS="${RUNSECS:-120}"

if [ -f "$LOG" ]; then
    echo "!! $LOG already exists -- pick a fresh tag. Two QEMUs writing one"
    echo "   filename is indistinguishable from a flaky kernel."
    exit 1
fi

LEFTOVER=$(tasklist 2>/dev/null | grep -ci qemu-system)
if [ "${LEFTOVER:-0}" != "0" ]; then
    echo "!! $LEFTOVER qemu process(es) still running. taskkill //F //IM"
    echo "   qemu-system-x86_64.exe first -- a leftover QEMU contaminates logs."
    exit 1
fi

KFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT -DTKAPP_CHROMEWIN"

echo "== forcing the flavour-sensitive objects to rebuild"
rm -f programs/chromewin/chromewin.o programs/chromewin/chromewin.elf
rm -f build/initrd.tar build/base.iso tobyOS.iso
touch src/kernel.c

echo "== building (kernel: $KFLAGS | chromewin: -DCW_SANDBOX)"
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="$KFLAGS" \
          PROG_EXTRA_CFLAGS="-DCW_SANDBOX" iso > "$BUILDLOG" 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' "$BUILDLOG" | head -20
    exit 1
fi

echo "== binary gate"
python -c "
import sys
cw = open('build/initrd/bin/chromewin','rb').read()
k  = open('tobyos.bin','rb').read()
fails = []
if b'--no-sandbox' in cw:      fails.append('staged chromewin STILL has --no-sandbox')
if b'sandbox build' not in cw: fails.append('staged chromewin has NO privilege-drop code')
if b'/bin/chromewin' not in k: fails.append('kernel does not reference /bin/chromewin (wrong launcher)')
for f in fails: print('   !!', f)
sys.exit(1 if fails else 0)
" || { echo "BINARY GATE FAILED -- the boot would test the wrong thing."; exit 1; }
echo "   ok  chromewin is a sandbox build and the kernel launches it"

echo "== running (${RUNSECS}s)"
timeout "$RUNSECS" "/c/Program Files/qemu/qemu-system-x86_64.exe" \
    -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -boot d -smp 4 -m 4096 -cpu qemu64,+smep,+smap \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1

taskkill //F //IM qemu-system-x86_64.exe > /dev/null 2>&1

echo
echo "== evidence"
grep -aE '\[chromewin\]|Running as root|\[ns\] clone|\[fork\] clone flags|credentials\.cc|\[proc\] pid=[0-9]+ .chrome|execve-argv|type=renderer' \
    "$LOG" | sed 's/^\[[0-9 ]*ms\] //' | head -40

RC=0
# NB: match 'dropped to uid=' and not the full '...=1000 gid=1000'. chromewin's
# stdout is CHUNKED through the [fd1] serial logger, so its lines interleave with
# kernel output and a longer pattern can straddle two records. That cost one
# run's verdict already: the drop had happened and the gate said it had not.
grep -aq 'dropped to uid=' "$LOG" || { echo "   !! no privilege drop line"; RC=1; }
grep -aq 'Running as root without --no-sandbox' "$LOG" && \
    { echo "   !! chromium still saw root"; RC=1; }
grep -aq 'credentials.cc' "$LOG" && \
    { echo "   !! sandbox CHECK failed (credentials.cc)"; RC=1; }
# Any clone that returns -1 is a namespace the sandbox asked for and did not
# get. Naming it here means the next blocker is in the verdict, not buried.
if grep -aqE '56\(clone\).*= -1 '  "$LOG"; then
    echo "   !! a clone() was REFUSED:"
    grep -aE '56\(clone\).*= -1 ' "$LOG" | sed 's/^\[[0-9 ]*ms\] //' | head -4
    RC=1
fi

# THE DECIDING CHECK: the browser has to actually come up. Everything above is
# a milestone on the way there, and milestones are not the destination.
if grep -aq 'bootstrap failed\|pipe closed / no reply' "$LOG"; then
    echo "   !! chromewin never bootstrapped -- the sandbox milestones above"
    echo "      passed and the browser still did not come up:"
    grep -aE 'FATAL:|Check failed' "$LOG" | sed 's/^\[[0-9 ]*ms\] //' | head -3
    RC=1
fi

FAULTS=$(grep -ac 'EXCEPTION\|user-mode fault\|PANIC' "$LOG")
echo
echo "== health: faults=$FAULTS   log=$LOG"

echo
[ "$RC" = "0" ] && echo "RESULT: GREEN" || echo "RESULT: RED"
exit $RC
