#!/bin/bash
# cwprofile.sh -- does the BROWSER PROFILE survive a reboot?
#
# usbprov.sh already proves the storage half (a stick enumerates, provisions,
# and /data comes back). It says nothing about the thing a user actually
# feels, which is chrome's own state: cookies, consent, logins. With a
# RAM-backed /data every boot hands chrome an empty --user-data-dir, so every
# site sees a first-time visitor forever -- consent screens return, nothing
# stays signed in, and a fresh cookie-less client issuing searches from one
# address is itself a bot shape (see the bot-gate history).
#
# So this asserts the browser-side claim end to end, on ONE persistent volume:
#
#   boot 1  blank disk -> provisioned -> chrome starts on an EMPTY profile
#           and writes to it            => "[chromewin] profile ...: FRESH"
#   boot 2  SAME disk, nothing else changed
#                                       => "[chromewin] profile ...: REUSED"
#
# The two-sided shape is the point. "REUSED on boot 2" alone would also be
# printed by a harness that never wiped anything, so boot 1 has to be seen
# starting from empty for boot 2 to mean what it says.
#
# It deliberately does NOT touch the repo's disk.img: that image carries
# whatever profile earlier runs left behind, which is exactly the ambiguity
# this test exists to remove. A blank scratch image is created per run.
#
# WHY WIKIPEDIA and not a search engine: it is the site this stack is known
# to load cleanly (667 KB article, zero JS errors), it sets cookies, and
# pointing an automated boot loop at google.com is what got this address
# flagged in the first place. Do not "improve" this by aiming it at a search
# engine.
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t; export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
QEMUIMG="/c/Program Files/qemu/qemu-img.exe"

URL="${URL:-https://en.wikipedia.org/wiki/HTTP_cookie}"
IMG=/c/t/cwprofile.img
RUNSECS="${RUNSECS:-200}"

# Quoting: single-quoting the backslash-quote is the only form that survives
# this shell AND make's recipe shell to reach clang (cwnet.sh paid for this).
PROGF='-DCW_URL=\"'"$URL"'\"'
KCF="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT -DTKAPP_BOOT -DTKAPP_CHROMEWIN"
KCF="$KCF -DPROVISION_BLANK_BOOT"

# --verdict-only re-scores the logs from a previous run. It MUST skip
# everything below: the first version placed this check after the build, so
# "just re-score it" rebuilt the kernel and deleted the scratch disk that the
# logs came from. A read-only flag that writes is a trap for the next reader.
VERDICT_ONLY=0
[ "$1" = "--verdict-only" ] && VERDICT_ONLY=1

if [ "$VERDICT_ONLY" = 0 ]; then
echo "=== [1/4] build (chromewin autostart + auto-provision the blank disk) ==="
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1; sleep 1
# make keys off mtime, not -D flags: a stale chromewin.o would navigate to the
# PREVIOUS url and report the PREVIOUS profile logic, inverting the verdict.
rm -f src/*.o
rm -f programs/chromewin/chromewin.o programs/chromewin/chromewin.elf
rm -f build/initrd.tar build/base.iso tobyOS.iso
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
     EXTRA_CFLAGS="$KCF" PROG_EXTRA_CFLAGS="$PROGF" \
     >logs/cwprofile.build.log 2>&1; then
    echo "BUILD FAILED -- tail:"; tail -25 logs/cwprofile.build.log; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAILED (no ISO)"; exit 1; }

# Gate on the BINARY, not on the flags having been typed. Both markers must be
# present or the verdict below is meaningless.
grep -qa -- "$URL" programs/chromewin/chromewin.elf \
    || { echo "  FAIL: $URL not in chromewin.elf (stale object)"; exit 1; }
grep -qa "profile %s: REUSED" programs/chromewin/chromewin.elf \
    || { echo "  FAIL: profile reporter not in chromewin.elf (stale object)"; exit 1; }
echo "  ok: chromewin.elf carries the url and the profile reporter"

echo "=== [2/4] blank 1 GiB scratch disk (repo disk.img untouched) ==="
rm -f "$IMG"
"$QEMUIMG" create -f raw "$IMG" 1G >/dev/null || exit 1
fi   # VERDICT_ONLY

run_boot () {   # $1 = serial log
    : > "$1"
    # No snapshot= : writes MUST land on $IMG, that is the whole experiment.
    "$QEMU" -cdrom tobyOS.iso -boot d \
      -drive file="$IMG",format=raw,if=none,id=hd0 \
      -device virtio-blk-pci,drive=hd0 \
      -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
      -accel whpx,kernel-irqchip=off \
      -smp 1 -m 8192 -cpu qemu64,+smep,+smap \
      -serial "file:$1" -no-reboot -display none &
    for i in $(seq 1 "$RUNSECS"); do
        grep -aq "\[chromewin\] profile " "$1" 2>/dev/null && break
        grep -aq 'KERNEL PANIC' "$1" 2>/dev/null && break
        sleep 1
    done
    # Keep running after the profile line so chrome actually WRITES its
    # profile out -- the line is printed before chrome is even spawned.
    sleep 60
    taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1
    sleep 2
}

# TRAP, paid for on the first run: the serial log does NOT contain the profile
# line as one contiguous string. Each printf is emitted in pieces split at its
# format conversions, so `[chromewin] profile /data/cr2: REUSED (1 entries...)`
# lands as `[chromewin] profile ` / `/data/cr2` / `: REUSED (` / `1` / ...,
# each interleaved with an `[fd1] len=N:` trace line. Grepping for the whole
# line therefore matches a FRAGMENT carrying no verdict word, and the first
# version of this script reported INCONCLUSIVE on a run that had in fact
# passed. Match the verdict TOKENS instead -- they sit inside literal runs and
# so are never split.
#
# And report those tokens rather than re-assembling the sentence: the second
# version tried, and printed "absentpresent: FRESH (: FRESH (", because every
# fragment appears twice (once in the [fd1] trace, once echoed) and grep -o
# emits them in file order. Garbled output in a verdict is worse than terse
# output -- it invites the reader to distrust the verdict itself.
prof_show () {   # $1 = log
    local state cdb
    if   prof_has "$1" "REUSED (" ; then state="REUSED"
    elif prof_has "$1" "FRESH ("  ; then state="FRESH"
    else                                state="<no profile report>"
    fi
    # The "cookie db " fragment and its value land on the SAME log line, so
    # this reads the real value rather than guessing from chunk lengths.
    cdb=$(tr '\r' '\n' < "$1" | grep -a "cookie db " \
            | grep -oE "present|absent" | tail -1)
    echo "  profile: ${state}${cdb:+, cookie db $cdb}"
}
prof_has () { tr '\r' '\n' < "$1" | grep -aq "$2"; }   # $2 = FRESH | "REUSED ("

# How many inodes the /data volume reported in use at mount. Boot 1 mounts a
# freshly formatted volume (1) and boot 2 mounts whatever chrome left behind,
# so this is a second, INDEPENDENT witness to persistence that does not go
# through chromewin's own reporting at all.
# NOTE: extract with one sed capture, not `grep -o "[0-9]*/"` -- that pattern
# also matches the bare slashes in "'/data'" (zero digits then '/'), so the
# first attempt printed two empty lines before the number.
data_inodes () {
    tr '\r' '\n' < "$1" \
      | sed -n "s|.*mounted '/data' on vblk0\.p1: \([0-9]*\)/.*|\1|p" | tail -1
}

verdict () {
    echo
    echo "=== VERDICT ==="
    echo "boot1:"; prof_show logs/cwprofile1.log
    echo "boot2:"; prof_show logs/cwprofile2.log
    i1="$(data_inodes logs/cwprofile1.log)"; i2="$(data_inodes logs/cwprofile2.log)"
    echo "/data inodes in use at mount: boot1=${i1:-?} boot2=${i2:-?}"
    echo
    if ! prof_has logs/cwprofile1.log "FRESH" ; then
        echo "INCONCLUSIVE: boot 1 did not start from an empty profile, so"
        echo "  'REUSED' on boot 2 would prove nothing."
    elif ! grep -aq "mounted '/data' on vblk0\.p1" logs/cwprofile2.log; then
        echo "FAIL: boot 2 did not mount /data from the provisioned partition,"
        echo "  so no profile could have survived."
    elif prof_has logs/cwprofile2.log "REUSED (" ; then
        echo "PASS: the profile written on boot 1 was still there on boot 2,"
        echo "  on a volume that was blank when boot 1 started."
    else
        echo "FAIL: /data persisted but chrome's profile did NOT come back."
    fi
    echo "logs: logs/cwprofile1.log logs/cwprofile2.log"
}

# Re-score an existing pair of logs without spending two boots on it. The logs
# ARE the evidence; the verdict is just arithmetic over them.
if [ "$VERDICT_ONLY" = 1 ]; then verdict; exit 0; fi

echo "=== [3/4] boot 1: blank disk -- chrome should start on an EMPTY profile ==="
run_boot logs/cwprofile1.log
tr '\r' '\n' < logs/cwprofile1.log \
  | grep -aE "PROVBOOT|mounted '/data'|is RAM-backed" | head -3
prof_show logs/cwprofile1.log

echo "=== [4/4] boot 2: SAME disk -- chrome should find that profile again ==="
run_boot logs/cwprofile2.log
tr '\r' '\n' < logs/cwprofile2.log \
  | grep -aE "mounted '/data'|is RAM-backed|tobyOS-data" | head -3
prof_show logs/cwprofile2.log

verdict
