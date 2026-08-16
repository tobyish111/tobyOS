#!/bin/bash
# Build the REAL-HARDWARE VALIDATION ISO: every Phase 3 harness, on one boot,
# with every result printed to the SERIAL CONSOLE.
#
# WHY THIS EXISTS, and why it is not build_usb.sh:
#
# The desktop ISO proves the things a human has to look at -- does it boot, does
# it draw, does a NON-ROOT login work, does the NIC get a lease. What it cannot
# do is report the Linux-arc test suites, because:
#   - src/serial.c is OUTPUT ONLY. There is no serial input path, so the
#     `admin@tobyOS:/#` prompt on the wire CANNOT be typed into;
#   - /bin/gui_term writes to its window via SYS_TERM_*, not to the serial log.
# So on a desktop ISO there is no way to run a command and see its output over
# serial. The harnesses run from the kernel and print with kprintf, which does
# reach the wire -- that is the whole point of this flavour.
#
# NOTE ON UID: a harness flavour logs in as root. That is FINE HERE and is not
# a regression of the non-root check -- build_usb.sh covers that case, and the
# two ISOs are deliberately split so each proves what it can actually prove.
#
# Output: tobyOS.iso -- write to USB with Rufus in DD mode.
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

# ===========================================================================
# THE FLAVOUR SET IS EXACTLY logs/lxnet.sh's, AND THAT IS NOT AN ACCIDENT.
#
# The first version of this script also enabled -DLXPOSIX_BOOT and
# -DLXCONTAINER_BOOT, on the reasoning that more coverage per flash is better.
# It was caught in QEMU before it shipped, and the combination is BROKEN:
#
#     [LXPOSIX] VERDICT: FAIL subtests=7/10
#     [LXNS]    VERDICT: FAIL subtests=14/21
#
# ...while each of those suites is green in its own gate (lxposix --full 23/23,
# lxnet LXNS 21/21). The failures are environmental, not code: the errno on the
# failing writes is 30 = EROFS, alongside `[mmap] user page alloc FAILED -- OOM`.
# The OCI capstone builds its own RAM-backed tobyfs volume for the Alpine
# rootfs early in the boot, and the later suites then run against a full /data.
#
# HARNESSES ARE NOT COMPOSABLE HERE. Each gate script uses one flavour for a
# reason. Shipping the combined build would have produced a large, alarming red
# on real hardware for a cause that has nothing to do with real hardware -- and
# burned a flash/boot cycle chasing it.
#
# To validate the other two, build them as their OWN images (one flag each):
#   -DLXPOSIX_BOOT      (expect [LXPOSIX] PASS 10/10, enosys_gaps=0)
#   -DLXCONTAINER_BOOT  (expect [LXCONTAINER] PASS setup=3/3 probe=0xff)
# ===========================================================================
FLAGS="-DFAST_BOOT -DQUICK_BOOT"
FLAGS="$FLAGS -DLXNS_BOOT"         # namespaces, ptrace, tmpfs, clone, netlink (C)
FLAGS="$FLAGS -DLXVETH_BOOT"       # veth across a namespace boundary
FLAGS="$FLAGS -DLXNETLINK_BOOT"    # the control plane: routes, bridge, NAT

# `make` keys off mtime, NOT -D flags, so objects from the previous flavour
# would be silently reused. Drop them.
echo "=== dropping objects from the previous flavour ==="
rm -f src/*.o

mkdir -p /c/t
echo "=== building ($FLAGS) ==="
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="$FLAGS" iso \
     >logs/build_usb_validate.log 2>&1; then
    echo "BUILD FAIL -- tail:"; tail -30 logs/build_usb_validate.log; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAIL (no ISO)"; exit 1; }
if [ tobyOS.iso -ot tobyos.bin ]; then
    echo "BUILD FAIL: tobyOS.iso is OLDER than tobyos.bin -- stale image"; exit 1
fi

# GATE ON THE BINARY. Every harness must actually be compiled in: a flavour that
# silently did not apply reports nothing, and "nothing on the wire" is
# indistinguishable from "the test passed quietly".
echo
echo "=== harness markers in tobyos.bin (all must be present) ==="
python -c "
d = open('tobyos.bin','rb').read()
need = {
  'LXNS'       : b'[LXNS] ==== namespace gate',
  'LXVETH'     : b'[LXVETH] ==== veth across',
  'LXNETLINK'  : b'[LXNETLINK] ==== the rtnetlink control plane',
}
# ...and the two that must NOT be here: they share /data with the suites above
# and starve them (EROFS + OOM). See the flavour comment at the top.
forbidden = {
  'LXPOSIX'    : b'[LXPOSIX] ==== POSIX acceptance gate',
  'LXCONTAINER': b'[LXCONTAINER] ==== ',
}
bad = [k for k, v in need.items() if v not in d]
for k in sorted(need):
    print(('  ok: ' if k not in bad else '  MISSING: ') + k)
extra = [k for k, v in forbidden.items() if v in d]
for k in extra:
    print('  PRESENT BUT MUST NOT BE: ' + k + ' (starves the others: EROFS/OOM)')
raise SystemExit(1 if (bad or extra) else 0)
" || { echo "  -- a harness did not compile in; not shipping this image"; exit 1; }

echo
echo "=== image ==="
ls -la tobyOS.iso
echo
echo "Write to USB with Rufus in DD mode, boot, and capture the whole serial log:"
echo "  plink -serial COM4 -sercfg 38400,8,n,1,N"
echo
echo "Expected verdict lines (grep 'VERDICT' in the capture):"
echo "  [LXVETH]    VERDICT: PASS subtests=6/6"
echo "  [LXNETLINK] VERDICT: PASS subtests=12/12"
echo "  [LXNS]      VERDICT: PASS subtests=21/21 skipped=0"
