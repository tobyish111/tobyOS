#!/bin/bash
# The shadow-suite account tools: useradd, userdel, usermod, groupadd,
# groupdel, groupmod, gpasswd, chsh, chfn, chage, vipw.
#
#   bash logs/accounts.sh
#
# A census against a Debian/Ubuntu base install found 85 commands missing
# beyond bash's builtins and coreutils; these eleven were the group whose
# absence actually blocked work, since tobyOS had a user database and a
# login but nothing to administer accounts with.
#
# NOT compared against GNU: shadow-utils is not installed on this host, and
# the behaviour that matters is what lands in /etc/passwd and /etc/group --
# which is checked directly, and is a stronger test than diffing help text.
#
# TWO THINGS THE TOOLS CANNOT DO, asserted here so they stay honest:
#   - /etc is ramfs. An existing file can be rewritten; a NEW file cannot be
#     created there, and nothing survives a reboot. So every change is
#     mirrored to /data/etc and the kernel restores it at boot -- the
#     @@mirror case proves the mirror is written.
#   - The kernel's login database (/data/users) is separate and userland has
#     no syscall to modify it, so useradd SAYS the account cannot log in yet
#     rather than leaving that to be discovered at a password prompt.
#
# Runs from the boot harness, not SSH -- see logs/coreutils.sh for why.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/accounts.log

MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 2
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

rm -f src/kernel.o
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
       EXTRA_CFLAGS=-DCOREUTILS_SELFTEST iso >logs/.buildlog.txt 2>&1
BUILD_RC=$?
# A FAILED BUILD USED TO LOOK LIKE A PASSING GATE. make's output went to
# /dev/null and the only check was "does an ISO exist" -- which a PREVIOUS
# run's ISO satisfies. One compile error therefore aborted make, left the
# stale image in place, and this gate booted it and reported confident
# results about source that was never in the build. Seen for real: a broken
# char literal in head.c silently froze three gates on an hour-old ISO.
if [ $BUILD_RC -ne 0 ]; then
    echo "ABORT: the build FAILED -- the ISO on disk is stale, so any"
    echo "       verdict below would have described code that is not in it."
    tail -25 logs/.buildlog.txt
    exit 1
fi
[ -f tobyOS.iso ] || { echo "ABORT: build produced no ISO"; exit 1; }

"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -boot d -smp 4 -m 5120 -cpu max \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

# The shared boot harness now runs six scripts (coreutils, extra,
# accounts, ssh, util, e2fs) and the ssh one alone takes ~40s of guest
# time. Waiting is cheap; a false ABORT costs a whole rebuild.
for i in $(seq 1 200); do grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break; sleep 2; done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"; exit 1
fi

# Strip the serial timestamp, then drop kernel diagnostics -- the console
# interleaves [elf]/[proc]/[fork] lines with the script's own output.
# A kernel diagnostic can interleave MID-LINE, not only at the start of
# one, so a filter that drops lines beginning with '[' leaves it behind and
# a perfectly correct tool looks broken. Cut an embedded diagnostic off
# where it starts, and swallow the blank line its own newline leaves.
kern_strip() {
    sed -E 's/^\[[0-9 ]+ms\] //' | grep -v '^\[' | awk '
        { if (match($0, /\[[0-9]+ ms\] \[[a-z_0-9]+\]/)) {
              frag = frag substr($0, 1, RSTART - 1); pending = 1; next }
          if (pending) {
              pending = 0
              if ($0 == "") { print frag; frag = ""; next }
              print frag $0; frag = ""; next }
          print }
        END { if (frag != "") print frag }' |
    sed -E 's/(.)(@@[a-z_])/\1\n\2/g'
}
OUT=$(sed -n '/@@start/,/@@END/p' "$LOG" | kern_strip | tr -d '\r')

if ! echo "$OUT" | grep -q "@@END"; then
    echo "ABORT: transcript incomplete"; echo "$OUT" | tail -3; exit 1
fi

section() { echo "$OUT" | awk -v k="@@$1" '$0==k{f=1;next} /^@@/{f=0} f'; }
PASS=0; FAIL=0
want() {
    local name="$1" label="$2" pat="$3" g
    g=$(section "$name" | tr '\n' '|')
    if echo "$g" | grep -q "$pat"; then echo "  ok   $label"; PASS=$((PASS+1))
    else echo "  FAIL $label -- got '$g' (wanted /$pat/)"; FAIL=$((FAIL+1)); fi
}
noo() {
    local name="$1" label="$2" pat="$3" g
    g=$(section "$name" | tr '\n' '|')
    if echo "$g" | grep -q "$pat"; then
        echo "  FAIL $label -- '$pat' still present in '$g'"; FAIL=$((FAIL+1))
    else echo "  ok   $label"; PASS=$((PASS+1)); fi
}

echo "=== useradd ==="
# The section begins with useradd's own NOTE line, so the record is not at
# the start of the joined text -- match the record itself, not position 0.
want useradd  "useradd writes a full passwd record" 'alice:x:1500:100:Test User:/home/alice:/bin/tsh'
want dupe     "useradd refuses a duplicate"         'already exists.*rc=9'
want autouid  "useradd picks the next free uid"     'bob:x:1[0-9][0-9][0-9]:'

echo "=== groups ==="
want groupadd       "groupadd writes a record"        'testers:x:400:'
want gpasswd_add    "gpasswd -a adds a member"        'testers:x:400:alice'
want gpasswd_twice  "adding twice does not duplicate" 'testers:x:400:alice|'
want gpasswd_del    "gpasswd -d removes a member"     'testers:x:400:|'
want groupmod       "groupmod -n renames"             'qa:x:400:'

echo "=== usermod / chsh / chfn ==="
want usermod_shell "usermod -s changes the shell"      'alice:.*:/bin/sh'
want usermod_G     "usermod -G sets membership"        'testers:x:400:alice'
want chsh_bad      "chsh refuses a missing shell"      'no such file.*rc=1'
want chsh_good     "chsh sets an existing shell"       'alice:.*:/bin/tsh'
want chfn          "chfn sets the real name"           'alice:x:1500:100:Alice A:'

echo "=== chage ==="
want chage "chage -l reports honestly (no /etc/shadow)" 'never'

echo "=== userdel ==="
want userdel               "userdel removes the record" '^0'
noo  userdel_purges_group  "userdel purges group membership" 'alice'
want groupdel              "groupdel removes the group" '^0'
want missing               "userdel on a missing user"  'does not exist.*rc=6'

echo "=== persistence ==="
want mirror "changes are mirrored to /data for the next boot" '^1'

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
