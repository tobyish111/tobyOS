#!/bin/bash
# Phase 3 slice 12 cut 4: THE NETWORKING CONTROL PLANE GATE, as one command.
#
#   bash logs/lxnet.sh            # the control-plane gate
#   bash logs/lxnet.sh --clean    # force a full rebuild first
#
# Exit status is the result: 0 = green, 1 = something failed.
#
# THREE INDEPENDENT LAYERS RUN IN ONE BOOT, on purpose:
#
#   LXNETLINK  kernel-side. The two things userspace cannot see: the INITIAL
#              namespace's reply down to its exact byte count (chrome's
#              AddressTrackerLinux and glibc's check_pf read it, and both fail
#              silently when it changes), and whether a netlink-created veth
#              pair really carries a frame.
#   LXVETH     the pre-existing veth gate, unchanged. It is here as a
#              REGRESSION check: cut 4 added an enforced admin-UP state that
#              every frame now passes through, and LXVETH sends frames without
#              ever bringing an interface up.
#   LXNS       userspace. /bin/linux-netlink drives real netlink over a real
#              socket, and a busybox `ip link add` witness drives it with a
#              tool written against Linux rather than against us.
#
# Each verdict is checked WITH ITS SUBTEST COUNT. "VERDICT: PASS" alone cannot
# see a subtest that never ran -- that exact hole reported LXNS green 17/17
# over a test whose binary had failed to stage.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

FLAGS="-DFAST_BOOT -DQUICK_BOOT -DLXNETLINK_BOOT -DLXVETH_BOOT -DLXNS_BOOT"
LOG=logs/lxnet.log
TIMEOUT=300

WANT_NETLINK=${WANT_NETLINK:-8}
WANT_VETH=${WANT_VETH:-6}
WANT_NS=${WANT_NS:-20}

MK=(make "CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc")

if [ "$1" = "--clean" ]; then
    echo "== make clean"
    "${MK[@]}" clean > logs/lxnet.build.log 2>&1
fi

echo "== building ($FLAGS)"
# EXTRA_CFLAGS changes trigger NO recompile in this Makefile -- both files that
# read these flags must be touched, or the previous flavour's objects link
# straight through and the run tests bits that do not contain the harness.
touch src/kernel.c src/rtnetlink.c
if ! "${MK[@]}" EXTRA_CFLAGS="$FLAGS" iso >> logs/lxnet.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/lxnet.build.log | head -30
    exit 1
fi

# Gate on the BINARY, never on the build log. Both markers come from this
# slice's sources, so their presence is proof the objects were rebuilt from
# them rather than carried over from a previous flavour.
python -c "
d = open('tobyos.bin','rb').read()
need = [b'[LXNETLINK] ==== the rtnetlink control plane',
        b'[NSIP] n=']
missing = [m for m in need if m not in d]
for m in missing:
    print('missing marker:', m.decode())
raise SystemExit(0 if not missing else 1)
" || { echo "!! tobyos.bin does not contain the cut-4 harness -- stale build,"
       echo "   or a marker string above was edited without updating this gate"
       exit 1; }

echo "== running (timeout ${TIMEOUT}s)"
rm -f "$LOG"
timeout "$TIMEOUT" "/c/Program Files/qemu/qemu-system-x86_64.exe" \
    -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -boot d -smp 4 -m 5120 \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1

echo
echo "== gate"
grep -a 'LXNETLINK\|LXVETH\|LXNS\|\[nl\]\|\[NSIP\]\|\[veth\]\|\[netns\]' "$LOG" \
    | sed 's/^\[[0-9 ]*ms\] //' | head -80

RC=0
check() {                       # check <TAG> <want-subtests>
    local tag="$1" want="$2" got
    grep -aq "$tag. VERDICT: PASS" "$LOG" || {
        echo "   !! $tag did not report PASS"; RC=1; }
    got=$(grep -a "$tag. VERDICT" "$LOG" | grep -o 'subtests=[0-9]*' \
          | head -1 | cut -d= -f2)
    if [ -z "$got" ]; then
        echo "   !! $tag produced no subtest count -- did it run at all?"
        RC=1
    elif [ "$got" != "$want" ]; then
        echo "   !! $tag ran $got subtests, expected $want"
        RC=1
    fi
}
echo
check LXNETLINK "$WANT_NETLINK"
check LXVETH    "$WANT_VETH"
check LXNS      "$WANT_NS"

# The host's own network must be untouched: the initial namespace's device list
# and addressing are net.c's, and cut 4 let that namespace hold control-plane
# devices for the first time. A DHCP lease in the same boot is the check.
# The log line is `[dhcp] ACK    bound:` -- COLUMN-ALIGNED, so the spaces are
# not single. A pattern assuming one space matched nothing on a boot where DHCP
# had plainly succeeded, and reported a network regression that had not
# happened.
grep -aqE '\[dhcp\] ACK +bound' "$LOG" || {
    echo "   !! no DHCP lease -- the host network path may have regressed"
    RC=1; }

HB=$(grep -ac '\[hb\]' "$LOG")
FAULTS=$(grep -ac 'EXCEPTION\|user-mode fault\|PANIC' "$LOG")
echo
echo "== health: heartbeats=$HB faults=$FAULTS"
[ "$HB" -lt 3 ] && { echo "   !! guest did not stay live"; RC=1; }
[ "$FAULTS" != "0" ] && { echo "   !! faults present"; RC=1; }

echo
[ "$RC" = "0" ] && echo "RESULT: GREEN" || echo "RESULT: RED"
exit $RC
