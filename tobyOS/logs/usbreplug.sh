#!/bin/bash
# usbreplug.sh -- does a REPLUGGED stick come back? The gate for the slice-122b
# usb_msc slot-revival fix.
#
# WHY THIS EXISTS: on the EliteDesk (2026-08-15) the user unplugged and
# replugged a stick while Disk Manager was open -- the most ordinary hardware
# action there is -- and the stick came back "UNKNOWN(unprobed)" until reboot:
#   [usb-msc] too many devices, ignoring slot 5
# usb_msc_unbind never freed the slot (and the table was 2 entries with two
# sticks attached, i.e. full from boot). The fix revives dead slots; this
# script proves the WHOLE cycle attach -> yank -> re-attach -> ONLINE using
# QMP device_del/device_add, which drives the same xHCI hot-plug path real
# hardware does.
#
#   PASS -> the reattached stick reaches "online" a SECOND time
#   FAIL -> the revive never happened / second online missing
#
# Uses the CURRENT tobyOS.iso (no special flags -- enumeration logs are
# always on). Does not touch the repo's disk.img.
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
QI="/c/Program Files/qemu/qemu-img.exe"
PY="${PY:-python}"
LOG=logs/usbreplug.log
STICK=/c/t/replug_stick.img
QMP_PORT=4470

[ -f tobyOS.iso ] || { echo "no tobyOS.iso -- build first"; exit 1; }
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1; sleep 1
rm -f "$STICK"; "$QI" create -f raw "$STICK" 1G >/dev/null || exit 1
: > "$LOG"

"$QEMU" -cdrom tobyOS.iso -boot d \
  -device qemu-xhci,id=xhci \
  -drive if=none,id=stickdrv,format=raw,file="$STICK" \
  -device usb-storage,bus=xhci.0,id=stickdev,drive=stickdrv \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -accel whpx,kernel-irqchip=off -smp 4 -m 4096 \
  -qmp tcp:127.0.0.1:$QMP_PORT,server,nowait \
  -serial "file:$LOG" -no-reboot -display none &

# Drive the yank/replug from python (QMP is JSON over TCP).
"$PY" - "$LOG" $QMP_PORT <<'EOF'
import json, socket, sys, time

log, port = sys.argv[1], int(sys.argv[2])

def wait_for(marker, timeout, why):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if marker in open(log, errors="replace").read():
                print("  ok: %s" % why); return True
        except OSError:
            pass
        time.sleep(1)
    print("  TIMEOUT waiting for: %s (%s)" % (why, marker)); return False

def qmp(sock, obj):
    sock.sendall((json.dumps(obj) + "\r\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        ch = sock.recv(1)
        if not ch: break
        buf += ch
    # TRAP (first run of this script): the failure was invisible because the
    # reply was discarded. Print every reply -- device_add failing with
    # "Drive 'stickdrv' not found" is the WHOLE diagnosis when a legacy
    # -drive is auto-deleted on unplug.
    r = buf.decode(errors="replace").strip()
    print("  qmp<- %s" % r[:160])
    return r

ok = wait_for("'usb0' online", 180, "stick online (first attach)")
if not ok: sys.exit(1)

s = socket.create_connection(("127.0.0.1", port), timeout=5)
buf = b""
while not buf.endswith(b"\n"): buf += s.recv(1)      # greeting
qmp(s, {"execute": "qmp_capabilities"})

print("=== yank ===")
qmp(s, {"execute": "device_del", "arguments": {"id": "stickdev"}})
if not wait_for("marked gone", 30, "kernel saw the detach"): sys.exit(1)
time.sleep(3)

print("=== replug ===")
# A legacy -drive is AUTO-DELETED when its device is hot-unplugged, so the
# original 'stickdrv' is gone by now -- recreate the backend first. NOTE the
# WINDOWS-style path: msys bash translates /c/... on a command line, but
# nothing translates it inside QMP JSON (same trap as -serial file: paths,
# see storage-provisioning).
qmp(s, {"execute": "blockdev-add",
        "arguments": {"node-name": "stick2", "driver": "raw",
                       "file": {"driver": "file",
                                 "filename": "C:/t/replug_stick.img"}}})
qmp(s, {"execute": "device_add",
        "arguments": {"driver": "usb-storage", "bus": "xhci.0",
                       "id": "stickdev2", "drive": "stick2"}})
if not wait_for("reviving dead slot", 30, "slot revived"): sys.exit(1)
# The second online line: count occurrences, need >= 2.
t0 = time.time()
while time.time() - t0 < 30:
    if open(log, errors="replace").read().count("'usb0' online") >= 2:
        print("  ok: stick ONLINE again after replug"); sys.exit(0)
    time.sleep(1)
print("  TIMEOUT: revive logged but stick never came online"); sys.exit(1)
EOF
RC=$?
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1
echo
if [ "$RC" = "0" ]; then echo "VERDICT: PASS -- yank + replug and the stick is usable again"
else echo "VERDICT: FAIL (rc=$RC) -- see $LOG"; fi
exit $RC
