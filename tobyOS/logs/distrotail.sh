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

# FORCE A KERNEL REBUILD. EXTRA_CFLAGS is not a file, so make cannot see
# that it changed: src/kernel.o built by the PREVIOUS gate -- with that
# gate's -D flags -- is newer than kernel.c and gets reused. Running
# logs/fsprobe.sh (which builds -DFSPROBE_BOOT, no -DCOREUTILS_SELFTEST)
# and then this gate booted a kernel with NO selftest in it at all: the
# harness never ran, and the only symptom was a boot that looked healthy
# and said nothing. Five other gates already did this; these did not.
rm -f src/kernel.o
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

# 800 s, not 440: the shared boot now runs eleven test scripts and the
# distro one alone spends ~40 s in ping, tcpdump, an FTP session and a
# debugger. The machine was alive and working at 438 s when this gate
# gave up on it.
for i in $(seq 1 400); do
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

echo "=== raw sockets: ping, tcpdump, clockdiff ==="
# None of these could exist before the kernel had SOCK_RAW; the reply also
# proves the raw socket delivers the WHOLE IP datagram, since ttl comes
# from the IP header.
ck ping_gateway "the gateway answers, with a ttl" 'bytes from 10\.0\.2\.2: icmp_seq=1 ttl=[0-9]+ time='
ck ping_gateway "and a second sequence number"   'icmp_seq=2'
ck ping_stats   "statistics report no loss"      '2 received, 0% packet loss'
ck ping_stats   "and an rtt line"                'rtt min/avg/max'
ck ping_badhost "an unresolvable name is refused" 'cannot resolve.*rc=[1-9]'
ck tcpdump_capture "the tap sees frames"          'frames: [1-9]'
ck tcpdump_capture "and reports what it captured" 'summary: 1'
ck clockdiff    "clockdiff answers or says why"   'delta=|did not answer'

echo
echo "=== ftp, against the ftpd running in this same boot ==="
ck ftp_setup    "the server has files to serve"   'served: [1-9]'
ck ftp_session  "the greeting arrives"            '220 tobyOS FTP server ready'
ck ftp_session  "login succeeds"                  '230 Logged in'
ck ftp_session  "PWD answers"                     '257 "/"'
ck ftp_session  "LIST shows the served file"      'greeting\.txt'
ck ftp_session  "SIZE reports the length"         '213 26'
ck ftp_session  "MKD creates a directory"         'subdir'
ck ftp_session  "RMD removes it"                  '250 Removed'
ck ftp_session  "DELE removes the file"           '250 Deleted'
ck ftp_session  "and the transfers report bytes"  '26 bytes received'
ck ftp_session  "including the upload"            'bytes sent for upload.txt'
# The transfer is only real if the bytes match.
eq ftp_bytes    "the downloaded file is identical"  'same: yes|'
eq ftp_upload   "and so is the uploaded one"        'uploaded: yes|identical: yes|'
eq ftp_deleted  "delete really deleted"             'greeting still there: no|'
ck ftp_noserver "connecting to a dead port is refused" 'cannot connect'
ck ftp_serverlog "the server logged that it was listening" 'listening on port 21'

echo "=== sudo: a real setuid-root program ==="
# Without the setuid bit the whole thing is decoration, so the bit itself
# is checked before anything it enables.
ck sudo_mode   "the binary is setuid"              '^-rws'
ck sudo_root   "root runs a command"               'uid=0'
ck sudo_root   "and -n succeeds without a prompt"  'rc=0'
ck sudo_list   "-l lists what the caller may run"  'may run the following'
ck sudo_nocmd  "an unknown command is reported"    'command not found.*rc=127'
ck sudo_nouser "an unknown target user is refused" 'unknown user'
ck sudo_sudoers "the shipped sudoers has the wheel rule" '^[1-9]'

echo
echo "=== ranlib ==="
# Only DEFINED globals belong in the index: internal_only is static, so a
# linker sent to liba.o for it would be sent for nothing.
ck ranlib_list  "lists the globals of both members" 'lib_add.*liba\.o'
ck ranlib_list  "including the second member's"     'lib_mul.*libb\.o'
ck ranlib_list  "and the counts"                    '2 member\(s\), 5 symbol\(s\)'
ck ranlib_write "writing the index reports it"      '5 symbol\(s\) indexed'
ck ranlib_write "and the archive grew by the index" 'grew: yes'
ck ranlib_idempotent "running twice changes nothing" 'stable: yes'
ck ranlib_notar "a non-archive is refused"          'not an archive.*rc=[1-9]'

echo
echo "=== addr2line ==="
# Pinned to what GNU addr2line answers for the same address in the same
# file -- taken from the host, not from this implementation.
eq a2l_line     "the line number matches GNU"       'dw.c:1|'
eq a2l_func     "-f names the function too"         'dw_helper|dw.c:1|'
ck a2l_unmapped "an unmapped address is ??:0"       '\?\?:0'
ck a2l_nodebug  "a file with no DWARF says so"      'no \.debug_line'

echo "=== gdb: real breakpoints over ptrace ==="
# The old /bin/debugger called PTRACE_TRACEME on ITSELF and its `break` only
# wrote an address into a table -- it never planted an int3 and never
# stopped anything. These assertions are what that could not have passed.
ck gdb_session "symbols are read from the target"   'Reading symbols'
ck gdb_session "a breakpoint is set on a symbol"    'Breakpoint 0 at 0x.*dbg_step'
ck gdb_session "and the target actually stops on it" 'Breakpoint 0, 0x.*in dbg_step'
ck gdb_session "registers can be read at the stop"  'rip 0x'
ck gdb_session "continuing hits it again"           'Breakpoint 0.*Breakpoint 0'
ck gdb_session "three calls, three stops"           'Breakpoint 0.*Breakpoint 0.*Breakpoint 0'
ck gdb_session "the first stop shows the argument"  'rdi 0x1'
# The exit CODE reaches the debugger: the tracee's status is collected by
# the tracer's own waitpid, not lost to a reap before it looked.
eq gdb_exitcode "and the exit code is collected at the end" '1|'
# Whether the target then runs to completion is answered separately, by
# running the SAME binary with no debugger attached -- otherwise a fixture
# that never exits and a debugger that cannot resume look identical.
ck gdb_fixture "the target exits on its own"        'standalone rc=7'
ck gdb_nosym   "an unknown symbol is refused"       'not defined'

echo "=== exec replaces the process, rather than spawning one ==="
# This is the check the old emulation could not have passed: it spawned the
# command and waited, so the `env` process was still there.
eq exec_replaces "no wrapper process is left behind" 'env wrappers: 0|sleep running: 1|'

echo "=== strace: real syscall tracing ==="
# The old /bin/strace traced nothing -- it called PTRACE_TRACEME on itself
# and, when that failed, printed a canned transcript. None of these could
# have passed against invented output, because they assert the calls this
# particular program actually makes.
ck strace_run    "syscalls are named, with arguments" '\((0x[0-9a-f]+, ){2}0x[0-9a-f]+\) = '
# busybox writes through writev, not write -- the assertion has to name
# the call the program actually makes.
ck strace_run    "and the output write is among them" 'writev\('
ck strace_run    "an ioctl on a pipe reports ENOTTY"  'ioctl.*errno 25'
ck strace_exit   "the exit status is reported"        'exited with 0'
ck strace_count  "-c totals the calls"                'total'
# The native dispatcher stops too, now: the fixture's exit(7) is a NATIVE
# syscall, named from the native table, and the status comes from waitpid.
ck strace_native "a NATIVE program is traced: its exit syscall is named" 'exit\(0x7'
ck strace_native "and its exit status is reported"                        'exited with 7'

echo "=== #! scripts ==="
# Both loaders were ELF/PE-only: ./script.sh was 127 from a shell that could
# run every binary on the system and none of the scripts. Each of these is a
# different path: spawn, execve, the one-argument rule, nesting, a Linux
# interpreter, and the three failures bash names.
eq shebang_spawn      "spawn hands a #! file to its interpreter"      'shebang ok world 1|rc=0|'
eq shebang_exec       "and so does execve (through env)"              'shebang ok via-execve 1|rc=0|'
eq shebang_optarg     "the rest of the line is ONE argument, then the script" 'hello there /tmp/sb2.sh a b|'
eq shebang_nested     "a script may be another script's interpreter"  'hello there /tmp/sb2.sh /tmp/sb3.sh z|'
eq shebang_busybox    "a Linux-personality interpreter works too"     'busybox sh: ok|rc=0|'
ck shebang_badinterp  "a missing interpreter: bash's message, 126"    'tsh: /tmp/sb4.sh: /no/such/interp: bad interpreter: No such file or directory.*rc=126'
ck shebang_loop       "a self-interpreting script is refused, not recursed" 'tsh: /tmp/sb5.sh: /tmp/sb5.sh: bad interpreter: Too many levels of symbolic links.*rc=126'
ck shebang_plain      "a text file with no #! runs as a script, as in bash" 'plain text yes /tmp/sb7.sh.*rc=0'
ck shebang_plain_pipe "also as a pipeline stage"                      'plain text piped /tmp/sb7.sh.*rc=0'
ck shebang_plain_linux "busybox sh does the same, given ENOEXEC"      'plain text bb /tmp/sb7.sh.*rc=0'
ck shebang_plain_execvp "and execvp runs it through the shell (POSIX)" 'plain text execvp /tmp/sb7.sh.*rc=0'
ck shebang_binary     "a binary that is no executable says so, 126"   'tsh: /tmp/sb8: cannot execute binary file: Exec format error.*rc=126'
ck shebang_noexec     "no execute bit: Permission denied, 126"        'tsh: /tmp/sb9.sh: Permission denied.*rc=126'

echo "=== /proc/stat: real accounting ==="
# Deltas across a busy loop in the guest, computed here. cpu user+system must
# grow while the shell loops; ctxt and intr grow across anything at all.
PB=$(section pstat_before); PA=$(section pstat_after)
BU=$(echo "$PB" | awk '$1=="cpu"{print $2+$4}'); AU=$(echo "$PA" | awk '$1=="cpu"{print $2+$4}')
AI=$(echo "$PA" | awk '$1=="cpu"{print $5}')
BC=$(echo "$PB" | awk '$1=="ctxt"{print $2}');   AC=$(echo "$PA" | awk '$1=="ctxt"{print $2}')
BN=$(echo "$PB" | awk '$1=="intr"{print $2}');   AN=$(echo "$PA" | awk '$1=="intr"{print $2}')
BT=$(echo "$PA" | awk '$1=="btime"{print $2}')
PR=$(echo "$PA" | awk '$1=="processes"{print $2}')
RN=$(echo "$PA" | awk '$1=="procs_running"{print $2}')
isnum() { echo "$1" | grep -Eq '^[0-9]+$'; }
delta() {   # delta <label> <before> <after> <min-growth>
    if isnum "$2" && isnum "$3" && [ "$3" -ge $(( $2 + $4 )) ]; then
        echo "  ok   $1 ($2 -> $3)"; PASS=$((PASS+1))
    else
        echo "  FAIL $1 -- '$2' -> '$3' (wanted growth >= $4)"; FAIL=$((FAIL+1))
    fi
}
delta "cpu user+system jiffies grew across the busy loop" "$BU" "$AU" 3
delta "ctxt grew"                                       "$BC" "$AC" 1
delta "intr grew"                                       "$BN" "$AN" 10
if isnum "$AI" && [ "$AI" -gt 0 ]; then echo "  ok   idle jiffies are counted too ($AI)"; PASS=$((PASS+1)); else echo "  FAIL idle jiffies '$AI'"; FAIL=$((FAIL+1)); fi
if isnum "$BT" && [ "$BT" -gt 1600000000 ]; then echo "  ok   btime is a real epoch time ($BT)"; PASS=$((PASS+1)); else echo "  FAIL btime '$BT'"; FAIL=$((FAIL+1)); fi
if isnum "$PR" && [ "$PR" -gt 100 ]; then echo "  ok   processes counts every spawn since boot ($PR)"; PASS=$((PASS+1)); else echo "  FAIL processes '$PR'"; FAIL=$((FAIL+1)); fi
if isnum "$RN" && [ "$RN" -ge 1 ]; then echo "  ok   procs_running is sampled ($RN)"; PASS=$((PASS+1)); else echo "  FAIL procs_running '$RN'"; FAIL=$((FAIL+1)); fi
ck pstat_loadavg "loadavg has three real averages and running/total" '^[0-9]+\.[0-9]{2} [0-9]+\.[0-9]{2} [0-9]+\.[0-9]{2} [0-9]+/[0-9]+ [0-9]+'
# vmstat's second row is a one-second interval: the timer alone is 100
# interrupts a second, so "in" below 100 would mean the column is invented.
VM2=$(section pstat_vmstat | sed -n '4p')
VIN=$(echo "$VM2" | awk '{print $11}'); VCS=$(echo "$VM2" | awk '{print $12}')
VUS=$(echo "$VM2" | awk '{print $13}'); VSY=$(echo "$VM2" | awk '{print $14}'); VID=$(echo "$VM2" | awk '{print $15}')
if isnum "$VIN" && [ "$VIN" -ge 100 ]; then echo "  ok   vmstat in >= 100/s from a 100 Hz tick ($VIN)"; PASS=$((PASS+1)); else echo "  FAIL vmstat in -- row '$VM2'"; FAIL=$((FAIL+1)); fi
if isnum "$VCS" && [ "$VCS" -ge 1 ]; then echo "  ok   vmstat cs is a rate ($VCS)"; PASS=$((PASS+1)); else echo "  FAIL vmstat cs -- row '$VM2'"; FAIL=$((FAIL+1)); fi
if isnum "$VUS" && isnum "$VSY" && isnum "$VID" && [ $((VUS + VSY + VID)) -ge 95 ] && [ $((VUS + VSY + VID)) -le 100 ]; then echo "  ok   vmstat us+sy+id is a percentage ($VUS+$VSY+$VID)"; PASS=$((PASS+1)); else echo "  FAIL vmstat cpu columns -- row '$VM2'"; FAIL=$((FAIL+1)); fi
ck pstat_vmstat "and it names the io columns as the missing ones" 'io columns .* are unavailable'

echo "=== chmod: symbolic modes ==="
eq chmod_sym       "the symbolic clauses land bit for bit"       '755|4745|444|444|755|1755|600|'
ck chmod_verbose   "-v reports a change the way GNU does"        'changed from 0600 \(rw-------\) to 0644 \(rw-r--r--\)\|rc=0\|.*to 0755 \(rwxr-xr-x\)'
eq chmod_recursive "-R walks the tree; X adds x only where due"  '640|711|600|'
ck chmod_bad       "a bad mode is refused by name, status 1"     "invalid mode: '\+q'\|rc=1"
ck chmod_bad       "a missing file gets GNU's message, status 1" "cannot access '/tmp/nonexistent': No such file or directory\|rc=1"

echo "=== printf: a NUL byte reaches a pipe ==="
ck printf_nul "%s\\0 writes the separator bytes (4 = x NUL y NUL)" '^4\|3\|'
ck printf_nul "and the byte lands where the escape was"             ' a \\0 b '

echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
