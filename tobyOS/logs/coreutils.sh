#!/bin/bash
# The 19 coreutils commands tobyOS was missing, against real GNU coreutils.
#
#   bash logs/coreutils.sh
#
# A census of the Debian/Ubuntu/RHEL/SUSE coreutils set (105 commands) against
# what the guest has on PATH -- purpose-built binaries in /bin plus ~390
# busybox applets -- found 86 present and these 19 absent:
#
#   b2sum basenc chcon csplit dir dircolors fmt join numfmt pathchk pinky
#   pr ptx runcon sha224sum sha384sum stdbuf tsort vdir
#
# They live in one binary dispatched on argv[0] (programs/user_coreutil),
# hard-linked to each name in the initrd exactly as busybox is. Staging the
# binary is NOT enough in this Makefile -- every name must also appear in the
# explicit tar list, or it never reaches the image and the command is simply
# missing at runtime.
#
# FOUR SSH SESSIONS, NOT THIRTY. Sessions are the fragile part: this server
# serves one at a time, so a single wedged session turned every later case
# into a banner-exchange timeout -- which reads as "all 27 commands are
# broken" when the truth is "one session hung". Batching also made the gate
# several times faster. See the chunk sizes below for why it is four and not
# one.
#
# GNU coreutils on this machine is the oracle wherever it can be. Three
# commands cannot be compared that way and are checked against literals:
# chcon/runcon need SELinux, which tobyOS has not got -- failing with that
# message is CORRECT, since succeeding silently would tell a script its
# labels were applied when nothing happened -- and stdbuf needs a preloaded
# libc shim, so it runs the command and reports that the buffering request
# had no effect.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/coreutils.log

MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 2
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

# NO SSH. The first version drove this over ~30 ssh sessions and then 4, and
# sessions were the wrong vehicle either way: this server serves one at a
# time, and the run churns dozens of short-lived processes -- the documented
# trigger for the open idle-stall flake. Runs wedged at a DIFFERENT case each
# time and reported as many as 27 phantom failures for one hung session.
#
# The kernel harness spawns /bin/tsh on the in-image script with console fds,
# so the transcript lands on the serial log. Same pattern as the editsave and
# filetypes gates, neither of which has ever stalled.
rm -f src/kernel.o
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"   make "CC=TMP='C:	' TEMP='C:	' clang" "HOST_CC=TMP='C:	' TEMP='C:	' gcc"        EXTRA_CFLAGS=-DCOREUTILS_SELFTEST iso >/dev/null 2>&1
[ -f tobyOS.iso ] || { echo "ABORT: build produced no ISO"; exit 1; }

"/c/Program Files/qemu/qemu-system-x86_64.exe"   -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk   -boot d -smp 4 -m 5120 -cpu max   -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

for i in $(seq 1 90); do grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break; sleep 2; done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"
    grep -a "COREUTILS" "$LOG" 2>/dev/null | head
    exit 1
fi

H=$(mktemp -d)
HOST_OUT=$(WORKDIR="$H" bash etc/coreutils_test.sh 2>&1)
rm -rf "$H"
# Strip the serial timestamps the kernel prefixes to console output.
# The console interleaves KERNEL diagnostics with the program's output --
# [elf] load OK, [proc] created, [sys_exit], [fork]. Strip the serial
# timestamp, then drop every remaining bracketed kernel line; what is left
# is exactly what the script wrote, which is what GNU is compared against.
kern_strip() { sed -E 's/^\[[0-9 ]+ms\] //' | grep -v '^\[' ; }
GUEST_OUT=$(sed -n '/@@sha224/,/@@END/p' "$LOG" | kern_strip | tr -d '\r')
EXTRA_OUT=$(sed -n '/@@dir/,/@@END/p' "$LOG" | kern_strip | tr -d '\r')

if ! echo "$GUEST_OUT" | grep -q "@@END"; then
    echo "ABORT: the guest transcript is incomplete"
    echo "$GUEST_OUT" | tail -3
    exit 1
fi

# section <text> <name>  -> the lines between @@name and the next @@
section() { echo "$1" | awk -v k="@@$2" '$0==k{f=1;next} /^@@/{f=0} f'; }

PASS=0; FAIL=0
cmp_case() {
    local name="$1" label="$2" b g
    # GNU coreutils on THIS host marks binary mode as "<hash> *file"; on
    # Linux -- and in the guest -- it is "<hash>  file". Normalise the
    # oracle rather than teach the guest a Windows-only quirk.
    b=$(section "$HOST_OUT" "$name" | sed -E 's/^([0-9a-f]{28,}) [*]/\1  /')
    g=$(section "$GUEST_OUT" "$name")
    if [ "$b" = "$g" ]; then echo "  ok   $label"; PASS=$((PASS+1))
    else
        echo "  FAIL $label"
        echo "         gnu : $(echo "$b" | tr '\n' '|')"
        echo "         toby: $(echo "$g" | tr '\n' '|')"
        FAIL=$((FAIL+1))
    fi
}
cmp_sorted() {
    local name="$1" label="$2" b g
    b=$(section "$HOST_OUT" "$name" | sort)
    g=$(section "$GUEST_OUT" "$name" | sort)
    if [ "$b" = "$g" ]; then echo "  ok   $label"; PASS=$((PASS+1))
    else
        echo "  FAIL $label"
        echo "         gnu : $(echo "$b" | tr '\n' '|')"
        echo "         toby: $(echo "$g" | tr '\n' '|')"
        FAIL=$((FAIL+1))
    fi
}
lit_case() {
    local name="$1" label="$2" want="$3" g
    # /bin/ls colours its output even when stdout is a PIPE, so dir and vdir
    # inherit escapes a real dir would not emit when redirected. That is ls's
    # own behaviour (sbase, third-party) and nothing dir can change from the
    # outside, so strip them rather than pretend the output is plain.
    g=$(section "$EXTRA_OUT" "$name" | sed -E 's/.\[[0-9;]*m//g' | tr '\n' '|')
    if [ "$g" = "$want" ]; then echo "  ok   $label"; PASS=$((PASS+1))
    else echo "  FAIL $label -- got '$g' want '$want'"; FAIL=$((FAIL+1)); fi
}

echo "=== checksums, against GNU ==="
cmp_case sha224 "sha224sum"
cmp_case sha384 "sha384sum"
cmp_case b2sum  "b2sum (BLAKE2b)"
echo "=== encodings ==="
cmp_case b64 "basenc --base64"
cmp_case b32 "basenc --base32"
cmp_case b16 "basenc --base16"
cmp_case rt  "basenc round trip"
echo "=== text ==="
cmp_case tsort   "tsort"
cmp_case fmt     "fmt -w 20"
cmp_case join    "join"
# join: ORDER-INSENSITIVE on purpose. GNU reads a whole key GROUP before
# emitting it, so its "is not sorted" diagnostic lands before the matched
# line; this join reads one line at a time and emits the match first. The
# DETECTION, the message, the line number and the exit status all agree --
# only the interleaving of stdout and stderr differs, and matching that would
# mean adopting GNU's group-reading structure for a malformed-input path.
cmp_sorted joinbad "join detects unsorted input (order-insensitive)"
cmp_case ptx     "ptx"
echo "=== numbers ==="
cmp_case numto   "numfmt --to=si"
cmp_case numtog  "numfmt --to=si (G)"
cmp_case numfrom "numfmt --from=si"
echo "=== files ==="
cmp_case csplit   "csplit"
cmp_case pr       "pr -t"
cmp_case pathbad  "pathchk rejects"
cmp_case pathok   "pathchk accepts"
echo "=== guest-only (no GNU oracle) ==="
lit_case dir       "dir lists"            'f1|'
lit_case vdir      "vdir long format"     '1|'
lit_case dircolors "dircolors sh"         '1|'
lit_case dircsh    "dircolors csh"        '1|'
lit_case pinky     "pinky lists a user"   '1|'
lit_case chcon     "chcon reports no SELinux" \
   "chcon: can't apply security context: SELinux is not enabled on this system|rc=1|"
lit_case runcon    "runcon reports no SELinux" \
   'runcon: SELinux is not enabled on this system|rc=1|'
lit_case stdbuf    "stdbuf still runs the command" 'ran|'

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
