#!/usr/bin/env bash
# distrotail.sh -- the last names a distro has that this tree did not:
# strip, dig, host, mkfs, domainname and cc.
#
#   bash logs/distrotail.sh
#
# What carries the weight here:
#
#   * strip is checked by RUNNING the binary it shrank. A stripped file
#     that no longer loads is the only interesting way for this tool to be
#     wrong, and its size says nothing about that. (The byte-level
#     comparison against GNU strip -- program headers unmoved, every
#     allocated section identical -- runs on the host, where readelf is.)
#
#   * dig and host ask the REAL resolver. DHCP writes /etc/resolv.conf at
#     ~6.7 s of boot and these run well after, so a "SERVER: 10.0.2.3"
#     line proves the tool read the file rather than falling back to its
#     compiled-in default -- which is the failure that would otherwise
#     look exactly like success.
#
#   * mkfs is a dispatcher, so what matters is that an unknown type is
#     reported as an unknown type, not as a missing file.

set -u
cd "$(dirname "$0")/.."

LOG=logs/distrotail.log
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
       EXTRA_CFLAGS=-DCOREUTILS_SELFTEST iso >logs/.buildlog.txt 2>&1
BUILD_RC=$?
# A FAILED BUILD USED TO LOOK LIKE A PASSING GATE -- see logs/coreutils.sh.
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

for i in $(seq 1 220); do
    grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break
    sleep 2
done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"
    exit 1
fi

# A kernel diagnostic can interleave MID-LINE, and the halves may be several
# diagnostics apart, so the prefix is held until the next surviving line
# says whether it was a whole line or the start of one. The trailing sed
# then puts each @@marker back on its own line -- a command that printed no
# final newline leaves the next `echo @@...` glued to its output.
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
OUT=$(sed -n '/@@dt_BEGIN/,/@@dt_END/p' "$LOG" | kern_strip | tr -d '\r')
if ! echo "$OUT" | grep -q "@@dt_END"; then
    echo "ABORT: the guest transcript is incomplete"
    echo "$OUT" | tail -5
    exit 1
fi

section() { echo "$OUT" | awk -v k="@@$1" '$0==k{f=1;next} /^@@[a-z_]/{f=0} f'; }

PASS=0; FAIL=0
eq() {
    local g; g=$(section "$1" | tr '\n' '|')
    if [ "$g" = "$3" ]; then echo "  ok   $2"; PASS=$((PASS+1));
    else echo "  FAIL $2 -- got '$g' (wanted '$3')"; FAIL=$((FAIL+1)); fi
}
ck() {
    local g; g=$(section "$1" | tr '\n' '|')
    if echo "$g" | grep -Eq "$3"; then echo "  ok   $2"; PASS=$((PASS+1));
    else echo "  FAIL $2 -- got '$g' (wanted /$3/)"; FAIL=$((FAIL+1)); fi
}

echo "=== strip: the shrunk binary still runs ==="
eq st_shrink     "the file got smaller"              'shrank: yes|'
eq st_run        "and it still loads and runs"       'dig (tobyOS) 1.0|'
# The copy is /tmp/dig2, and dig prints its own argv[0] -- so dig2 is the
# right answer here, and seeing it proves the -g output ran.
eq st_debug_only "-g leaves a working binary too"    'dig2 (tobyOS) 1.0|'
ck st_notelf     "a non-ELF file is refused"         'not a little-endian ELF64 file.*rc=[1-9]'
eq st_intact     "and is left byte-for-byte alone"   'this is not an elf|'

echo
echo "=== dig / host: the real resolver, over UDP ==="
# 10.0.2.3 is QEMU's user-mode nameserver, and it is in /etc/resolv.conf
# because DHCP put it there. Matching it proves the file was read.
ck dig_server "reads the nameserver from resolv.conf" 'SERVER: 10\.0\.2\.3#53'
ck dig_short  "+short prints just the address"        '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'
eq dig_answer "an answer section is present"          '1|'
ck dig_nx     "a bad name comes back NXDOMAIN"        'status: NXDOMAIN'
ck dig_mx     "-t MX returns MX records"              '^[1-9]'
ck host_a     "host says 'has address'"               'example\.com has address [0-9]+\.'
ck host_rev   "a bare address reverses"               'domain name pointer'
ck host_nx    "host reports NXDOMAIN plainly"         'not found: 3\(NXDOMAIN\)'

echo
echo "=== mkfs: the dispatcher util-linux's mkfs actually is ==="
eq mkfs_version "reports itself"                      'mkfs (tobyOS) 1.0|'
ck mkfs_unknown "an unknown type names the ones that exist" \
                'no builder for this type.*ext2.*rc=127'

echo
echo "=== domainname ==="
ck dn_read "reads the UTS domain"                     '.'
ck dn_set  "setting is refused, with the reason"      'no setdomainname.*rc=[1-9]'

echo
echo "=== cc ==="
eq cc_same    "cc is the compiler under its usual name" 'cc: present|same size as tcc: yes|'
# This gate found tobyOS's own compiler page-faulting on EVERY compile:
# TccState is megabytes and it was a stack local, so the memset that zeroes
# it walked off the end of the stack. One `static` fixed it, and these
# assertions are what proves it.
ck cc_compile_void  "int main(void) compiles"            'built: yes'
ck cc_compile_empty "int main() compiles"                 'built: yes'
ck cc_run           "and the emitted binary runs and returns 7" 'ran rc=7'

echo
echo "=== at / batch / atq / atrm ==="
ck at_queue    "at queues a job and names the time"  '^job [0-9]+ at '
ck at_list     "atq lists it"                        '^[0-9]+ '
ck at_badtime  "an unparseable time is refused"      'cannot understand that time.*rc=[1-9]'
ck at_t        "-t CCYYMMDDhhmm is accepted"         '^job [0-9]+ at .*2099'
ck at_empty    "an empty job is not queued"          'no commands given'
ck at_hhmm     "HH:MM schedules for that time"       '23:30:00'
# The whole point of the tool: the command runs later, without a daemon.
ck at_runs     "a job scheduled for now actually runs" 'it-ran'
# And the whole point of atrm: the runner RE-READS the spool, so removing
# the file genuinely cancels the job instead of only hiding it from atq.
ck at_cancel   "atrm cancels it, and atq forgets it" 'ran: no.*still queued: 0'
ck at_rmmissing "removing a job that is not there says so" 'cannot find job 999'
ck batch       "batch queues without a time"         '^job [0-9]+ at '

echo "=== setpci and /sys/bus/pci/devices/*/config ==="
# There was no per-device config file at all before this: `lspci -x` had
# nothing to dump and setpci had nothing to read.
eq pci_configfile "config is a full 256-byte header"  '256|'
eq setpci_vendor  "VENDOR_ID reads 8086"              '8086|'
eq setpci_device  "DEVICE_ID reads 100e"              '100e|'
eq setpci_class   "CLASS_DEVICE reads 0200 (ethernet)" '0200|'
# The sysfs bytes are little-endian, so 8086 reads back as 86 80.
ck setpci_agrees  "the dump starts with the vendor id" '^00: 86 80 0e 10'
ck setpci_agrees  "and the file has a full header"     'header bytes: 16'
eq setpci_bydev   "-d vendor:device selects the same one" '8086|'
ck setpci_dump    "--dump prints the header"          '^00: 86 80 0e 10'
ck setpci_write   "a write takes effect"              'now: 20'
ck setpci_refresh "the sysfs snapshot moves with it"  'changed: yes'
ck setpci_refresh "and moves back"                     'returned: yes'
ck setpci_badreg  "an unknown register is refused"    'unknown register.*rc=[1-9]'
ck setpci_nowidth "a numeric offset needs a width"    'needs a width suffix'
ck setpci_nodev   "an unenumerated address is refused" 'no such PCI function'
ck setpci_allwrite "writing to every device is refused" 'refusing to write to every device'

echo "=== systemctl / service: what the registry actually exposes ==="
ck svc_list        "list-units prints the header"        '^UNIT +LOAD +ACTIVE'
ck svc_count       "and a unit count"                    'unit\(s\) listed'
ck svc_isactive    "is-active answers for a real unit"   'unit: [a-z]'
ck svc_show        "show prints unit properties"         'Id=.*\.service'
# There is no start syscall. Saying so beats printing nothing and exiting 0.
ck svc_refuse_start "start is refused, with the reason"  'cannot start a service.*rc=[1-9]'
ck svc_refuse_enable "so is enable"                      'cannot enable a service'
ck svc_unknown     "an unknown unit exits 4"             'not found.*rc=4'
ck service_all     "service --status-all lists them too" '^UNIT +LOAD'
# svcctl's start USED to print "Requested start of X" and exit 0.
ck svcctl_honest   "svcctl no longer claims to have acted" '^1.*rc=[1-9]'

echo
echo "=== getent ==="
ck getent_passwd  "passwd by name"                       '^root:x:0:0:'
ck getent_uid     "passwd by uid"                        '^root:x:0:0:'
ck getent_group   "group by name"                        '^wheel:x:10:'
ck getent_hosts   "hosts answers for localhost"          '127\.0\.0\.1'
ck getent_missing "a name that is not there exits 2"     'rc=2'
ck getent_unknown "a database this system lacks says so" 'unknown database.*rc=2'
ck getent_all     "no key dumps the whole database"      '^[1-9]'

echo
echo "=== netcat ==="
eq netcat "netcat is nc under its long name" 'netcat: present|same as nc: yes|'

echo "=== compress / uncompress: the .Z format, not a gzip alias ==="
eq z_roundtrip "a .Z round trip returns the same bytes" \
               'made: yes|smaller: yes|same size: yes|'
# 1f 9d is compress(1)'s magic; cksum of those two bytes is a fixed number.
# 1f 9d is compress(1)'s magic; its POSIX cksum is a fixed number, so this
# pins the actual bytes rather than merely "something was written".
eq z_magic     "the output carries the .Z magic"        '1871826912|'
ck z_zcat      "zcat recognises .Z by its magic"        '^[1-9]'
ck z_notz      "a non-.Z file is refused"               'not in compress \(\.Z\) format.*rc=[1-9]'

echo "=== nc: TCP by default, as netcat has always been ==="
ck nc_tcp_open   "an open TCP port is reported open"  'port 22 \(tcp\) open.*rc=0'
ck nc_tcp_closed "a closed one is refused"            'failed.*rc=[1-9]'

echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
