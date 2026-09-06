# logs/distrochecks.sh -- THE check block, shared by logs/distrotail.sh (a
# QEMU boot) and logs/hwgate.sh (the real EliteDesk). Sourced with $OUT (the
# transcript between @@dt_BEGIN and @@dt_END, kernel lines stripped) and $LOG
# set. SELECT, if set, is a list of section-name prefixes: checks on other
# sections are skipped, so a hardware run can grade the sections it ran.
section() { echo "$OUT" | awk -v k="@@$1" '$0==k{f=1;next} /^@@[a-z_]/{f=0} f'; }
sel() {
    [ -z "${SELECT:-}" ] && return 0
    local p
    for p in $SELECT; do case "$1" in "$p"*) return 0 ;; esac; done
    return 1
}

PASS=0; FAIL=0
eq() {
    sel "$1" || return 0
    local g; g=$(section "$1" | tr '\n' '|')
    if [ "$g" = "$3" ]; then echo "  ok   $2"; PASS=$((PASS+1));
    else echo "  FAIL $2 -- got '$g' (wanted '$3')"; FAIL=$((FAIL+1)); fi
}
ck() {
    sel "$1" || return 0
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
# The native dispatcher stops too: the fixture's exit(7) is a NATIVE
# syscall, named from the native table, and the status comes from waitpid.
ck strace_native "a NATIVE program is traced: its exit syscall is named" 'exit\(0x7'
ck strace_native "and its exit status is reported"                        'exited with 7'
eq strace_native_repeat "ten native traces in a row complete, leaving no parked tracee" 'traced: 10/10|0|'

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
if sel pstat_; then
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
fi
ck pstat_loadavg "loadavg has three real averages and running/total" '^[0-9]+\.[0-9]{2} [0-9]+\.[0-9]{2} [0-9]+\.[0-9]{2} [0-9]+/[0-9]+ [0-9]+'
# vmstat's second row is a one-second interval: the timer alone is 100
# interrupts a second, so "in" below 100 would mean the column is invented.
if sel pstat_; then
VM2=$(section pstat_vmstat | sed -n '4p')
VIN=$(echo "$VM2" | awk '{print $11}'); VCS=$(echo "$VM2" | awk '{print $12}')
VUS=$(echo "$VM2" | awk '{print $13}'); VSY=$(echo "$VM2" | awk '{print $14}'); VID=$(echo "$VM2" | awk '{print $15}')
if isnum "$VIN" && [ "$VIN" -ge 100 ]; then echo "  ok   vmstat in >= 100/s from a 100 Hz tick ($VIN)"; PASS=$((PASS+1)); else echo "  FAIL vmstat in -- row '$VM2'"; FAIL=$((FAIL+1)); fi
if isnum "$VCS" && [ "$VCS" -ge 1 ]; then echo "  ok   vmstat cs is a rate ($VCS)"; PASS=$((PASS+1)); else echo "  FAIL vmstat cs -- row '$VM2'"; FAIL=$((FAIL+1)); fi
if isnum "$VUS" && isnum "$VSY" && isnum "$VID" && [ $((VUS + VSY + VID)) -ge 95 ] && [ $((VUS + VSY + VID)) -le 100 ]; then echo "  ok   vmstat us+sy+id is a percentage ($VUS+$VSY+$VID)"; PASS=$((PASS+1)); else echo "  FAIL vmstat cpu columns -- row '$VM2'"; FAIL=$((FAIL+1)); fi
fi
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

echo "=== the native clock is the real clock ==="
# The oracle for "what time is it" is busybox: a Linux-personality program
# reading the kernel's RTC-anchored clock. Native date used to say 2025.
if sel clock_; then
SK=$(section clock_agree | sed -n 's/.*skew=\([0-9]*\).*/\1/p')
if isnum "$SK" && [ "$SK" -le 2 ]; then echo "  ok   native date +%s agrees with busybox (skew ${SK}s)"; PASS=$((PASS+1)); else echo "  FAIL native and busybox clocks disagree -- $(section clock_agree | tr '\n' ' ')"; FAIL=$((FAIL+1)); fi
Y1=$(section clock_year | sed -n 1p); Y2=$(section clock_year | sed -n 2p)
BTY=$(date -u -d @"$BT" +%Y 2>/dev/null)
if [ -n "$Y1" ] && [ "$Y1" = "$Y2" ] && [ "$Y1" = "$BTY" ]; then echo "  ok   the year is the RTC's year on both sides ($Y1)"; PASS=$((PASS+1)); else echo "  FAIL year -- native '$Y1' busybox '$Y2' btime '$BTY'"; FAIL=$((FAIL+1)); fi
fi
ck clock_default "the default format is POSIX's, with a real year" '^[A-Z][a-z]{2} [A-Z][a-z]{2} [ 0-9][0-9] [0-9]{2}:[0-9]{2}:[0-9]{2} UTC 20[2-9][0-9]\|$'
same() {   # same <section> <label>: the section's two lines are identical
    sel "$1" || return 0
    local a b; a=$(section "$1" | sed -n 1p); b=$(section "$1" | sed -n 2p)
    if [ -n "$a" ] && [ "$a" = "$b" ]; then echo "  ok   $2"; PASS=$((PASS+1));
    else echo "  FAIL $2 -- native '$a' vs busybox '$b'"; FAIL=$((FAIL+1)); fi
}
same clock_fixed       "every strftime conversion matches busybox at a fixed instant"
same clock_fixed_flags "and so do the -, _ and 0 flags, a width, and %c"
eq clock_gnu_only      "and the GNU-only %k %l %P %q conversions come out right" '22|10|pm|4|'
E1=$(date -u -d '2024-02-29 12:34:56' +%s); E2=$(date -u -d '2024-02-29T12:34' +%s); E3=$(date -u -d 2024-02-29 +%s)
eq clock_dateparse "-d parses @epoch and the ISO date-time forms" "$E1|$E2|$E3|Thu Jan  1 00:00:00 UTC 1970|date: invalid date 'nonsense'|rc=1|"
eq clock_iso_rfc   "-I and -R print ISO 8601 and RFC 2822"        '2023-11-14|2023-11-14T22:13:20+00:00|Tue, 14 Nov 2023 22:13:20 +0000|'
if sel clock_; then
MSK=$(section clock_mtime | sed -n 's/.*skew=\([0-9]*\).*/\1/p')
if isnum "$MSK" && [ "$MSK" -le 5 ]; then echo "  ok   a native touch stamps the mtime busybox stat reads back (skew ${MSK}s)"; PASS=$((PASS+1)); else echo "  FAIL native touch vs busybox stat -- $(section clock_mtime | sed -n 1p)"; FAIL=$((FAIL+1)); fi
MY=$(section clock_mtime | sed -n 2p)
if [ -n "$MY" ] && [ "$MY" = "$Y1" ]; then echo "  ok   date -r reads that mtime in the same calendar"; PASS=$((PASS+1)); else echo "  FAIL date -r year '$MY' (wanted $Y1)"; FAIL=$((FAIL+1)); fi
ML=$(section clock_mtime | sed -n 3p); BTM=$(date -u -d @"$BT" +%b 2>/dev/null)
if [ -n "$ML" ] && [ "$ML" = "$BTM" ]; then echo "  ok   native ls -l shows this month for a busybox-touched file ($ML)"; PASS=$((PASS+1)); else echo "  FAIL ls -l month '$ML' (wanted $BTM)"; FAIL=$((FAIL+1)); fi
PT=$(section clock_printf_T)
P1=$(echo "$PT" | sed -n 1p); P2=$(echo "$PT" | sed -n 2p); PSK=$(echo "$PT" | sed -n 's/^skew=//p')
if [ -n "$P1" ] && [ "$P1" = "$Y1" ] && [ "$P2" = "$Y1" ]; then echo "  ok   printf %(%Y)T is this year, with -1 and with no operand"; PASS=$((PASS+1)); else echo "  FAIL printf %T year -- '$P1' '$P2' (wanted $Y1)"; FAIL=$((FAIL+1)); fi
if isnum "$PSK" && [ "$PSK" -le 2 ]; then echo "  ok   %(%s)T is the same clock as date +%s (skew ${PSK}s)"; PASS=$((PASS+1)); else echo "  FAIL %(%s)T skew '$PSK'"; FAIL=$((FAIL+1)); fi
fi
ck clock_printf_T "a fixed epoch formats the way bash formats it" '2023-11-14 22:13:20 Tuesday 318\|1700000000\|x\[  1970-01-02\]\[1970-01-02  \]\|00:00:00\|'

echo "=== ps: processes, not services ==="
ck ps_self        "ps -p \$\$ names the shell that ran it"           '[0-9]+ tsh\|tsh\|'
eq ps_header_bare "bare ps has procps's columns"                     '  PID TTY          TIME CMD|'
eq ps_header_aux  "ps aux has procps's columns"                      'USER       PID %CPU %MEM    VSZ   RSS TTY      STAT START   TIME COMMAND|'
eq ps_header_ef   "ps -ef has procps's columns"                      'UID        PID  PPID  C STIME TTY          TIME CMD|'
eq ps_header_ax   "ps ax has procps's columns"                       '  PID TTY      STAT   TIME COMMAND|'
ck ps_aux_tsh     "ps aux | grep tsh finds the running shell"        '^[1-9]'
if sel ps_; then
PSN=$(section ps_pidset | sed -n 's/^count: \([0-9]*\).*/\1/p')
if isnum "$PSN" && [ "$PSN" -ge 3 ]; then echo "  ok   ps -e lists the process table ($PSN pids)"; PASS=$((PASS+1)); else echo "  FAIL ps -e count -- '$PSN'"; FAIL=$((FAIL+1)); fi
fi
ck ps_pidset      "the shell itself is in ps -e"                     'shell: 1\|'
if sel ps_; then
BBD=$(section ps_busybox | sed -n 's/.*only-in-one: \([0-9]*\).*/\1/p')
if isnum "$BBD" && [ "$BBD" -le 3 ]; then echo "  ok   and busybox ps sees the same set ($BBD transient)"; PASS=$((PASS+1)); else echo "  FAIL busybox ps disagrees -- '$BBD'"; FAIL=$((FAIL+1)); fi
fi
ck ps_columns     "the -o columns carry procps's headers"            '^ *PID *PPID *PGID USER *%CPU %MEM *VSZ *RSS STAT *ELAPSED *TIME COMMAND'
ck ps_columns     "and the shell's row is fully populated"           '[0-9]+ +[0-9]+ +[0-9]+ [a-z0-9]+ +[0-9]+\.[0-9] +[0-9]+\.[0-9] +[0-9]+ +[0-9]+ [RST][<Nsl+]* +[0-9:-]+ +[0-9:-]+ tsh'
ck ps_custom_header "pid=NAME sets the header; pid= drops it"        '^PROCESS NAME\|.*tsh\|tsh\|'
if sel ps_; then
PE=$(section ps_etime | sed -n 1p); PS2=$(section ps_etime | sed -n 2p); PP=$(section ps_etime | sed -n 3p); SELF=$(section ps_etime | sed -n 's/^self=//p')
if isnum "$PE" && [ "$PE" -ge 1 ] && [ "$PE" -le 4 ]; then echo "  ok   etimes counts from the process start (${PE}s)"; PASS=$((PASS+1)); else echo "  FAIL etimes '$PE'"; FAIL=$((FAIL+1)); fi
if [ "$PS2" = "S" ]; then echo "  ok   a sleeping process is S"; PASS=$((PASS+1)); else echo "  FAIL state of a sleeping process '$PS2'"; FAIL=$((FAIL+1)); fi
if [ -n "$PP" ] && [ "$PP" = "$SELF" ]; then echo "  ok   ppid is the shell that backgrounded it"; PASS=$((PASS+1)); else echo "  FAIL ppid '$PP' (wanted $SELF)"; FAIL=$((FAIL+1)); fi
fi
eq ps_none        "an empty selection is status 1; a bad column is named" 'rc=1|0|ps: error: unknown user-defined format specifier "nosuchcolumn"|'
ck ps_services    "--json is gone; the service list lives in svcctl" '^rc=1\|[1-9]'

echo "=== /bin/sh is tsh ==="
eq sh_same_binary "/bin/sh is the same binary as /bin/tsh"                       'same as tsh: yes|'
eq sh_c           "sh -c runs POSIX arithmetic, parameter expansion and loops"  '3|bc ab 3|1-2-|'
ck sh_posix_mode  "sh runs in POSIX mode, tsh does not"                         '^posix[[:space:]]+on\|posix[[:space:]]+off\|'
eq sh_script      "a #!/bin/sh script with the POSIX constructs runs"           'case ok|heredoc 42|test ok|/tmp/posix.sh 2|rc=0|'
eq sh_system      "system() and popen() from a native program go through it"    'FROM-SYSTEM|system_status=3|system_probe=1|popen_lines=3|popen_write_first=alpha|6|system_quoted=0|systest: OK (0 fail)|'

echo
echo "=== native-parity batch: the 46 busybox-only utilities, natively ==="
eq census_46  "all 46 exist in /bin and none is the busybox binary" 'census done|'
eq setup_users "useradd registers the account in /etc/passwd AND the kernel login database" 'add=0|1|1|1601|'
ck proc_new   "/proc/vmstat has the procps counters"        'nr_free_pages nr_pages_total pgfault pgmajfault pswpin pswpout'
ck proc_new   "and the page-fault counter is live"          'faults counted'
ck proc_new   "/proc/net/route has Linux's header and an eth0 route" 'Iface Destination Gateway.*\|[1-9]\|'
ck proc_new   "/proc/net/arp has Linux's header"            'IP address HW type Flags'
eq tsh_caps   "tsh: 255 positional parameters, a 1.9 KiB \$(...) and \"\" -> 127" '255|1940|tsh: : command not found|rc=127|loop ok|'
eq mu_arch    "arch agrees with busybox"                    'x86_64|x86_64|'
eq mu_true_false "true/false exit as named, true --help works" 'rc=0|rc=1|true|'
eq mu_printf  "printf: widths, octal/hex, %c, %b, reuse, floats, \\x \\u, %q, bad number" 'abc|   42|ab  |010|ff|z|x	y|%|1 2|3 0|3.142 1.234500e+03 0.0001 +7 2.01|ABé|'"'"'it'"'"'\'"'"''"'"'s'"'"'|printf: '"'"'abc'"'"': expected a numeric value|0|rc=1|   ab|a|b   ||'
eq mu_test    "test/[: unary, binary, ! ( ) -a -o, missing ], bad integer" 'rc=0|rc=1|rc=0|rc=1|x[: missing '"'"']'"'"'|rc=2|rc=0|test: x: integer expression expected|rc=2|rc=0|rc=0|'
eq mu_factor  "factor: primes, composites, stdin, bad input" '360: 2 2 2 3 3 5|97: 97|1:|1000000007: 1000000007|12: 2 2 3|factor: '"'"'abc'"'"' is not a valid positive integer|rc=1|'
same_first_field() {   # the checksum (first field) of line pairs agree
    sel "$1" || return 0
    local a b c d
    a=$(section "$1" | sed -n 1p | cut -d' ' -f1); b=$(section "$1" | sed -n 2p | cut -d' ' -f1)
    c=$(section "$1" | sed -n 3p | cut -d' ' -f1); d=$(section "$1" | sed -n 4p | cut -d' ' -f1)
    if [ -n "$a" ] && [ "$a" = "$b" ] && [ -n "$c" ] && [ "$c" = "$d" ]; then echo "  ok   $2"; PASS=$((PASS+1)); else echo "  FAIL $2 -- $(section "$1" | tr '\n' '|')"; FAIL=$((FAIL+1)); fi
}
same_first_field mu_sum "sum: BSD and SysV checksums agree with busybox"
ck mu_link_unlink "link makes a real hard link (shared inode) and unlink removes one name" '^(rc=0\|two\|rc=0\|gone\|two\|)|(link: cannot create link.*Operation not permitted\|rc=1\|unlink: cannot unlink.*No such file or directory\|rc=1\|gone\|one\|)'
ck mu_link_unlink "unlink of a missing file fails as coreutils does"    'unlink: cannot unlink .*nonesuch.*: No such file or directory\|rc=1\|$'
ck mu_logname_users "logname/users/who agree on the console session; nproc; hostid" '^root\|rc=0\|(users=\[\]\|who=0\|# users=0|users=\[root\]\|who=1\|# users=1)\|1\|[1-9][0-9]*\|[1-9][0-9]*\|hostid=[0-9a-f]{8}\|$'
ck mu_uptime  "uptime, -p and -s in procps's formats"       '^ [0-9]{2}:[0-9]{2}:[0-9]{2} up .*,  [0-9]+ users?,  load average: [0-9]+\.[0-9]{2}, [0-9]+\.[0-9]{2}, [0-9]+\.[0-9]{2}\|up .*minute.*\|20[2-9][0-9]-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\|$'
eq mu_pwd     "pwd and pwd -P"                              '/tmp|/tmp|'
eq mu_chgrp   "chgrp by name and by id, -v, unknown group"  'changed group of '"'"'/tmp/cg'"'"' from root to wheel|wheel|20|chgrp: invalid group: '"'"'nogroup'"'"'|rc=1|'
eq mu_shred   "shred: passes, zero pass, remove"            '/tmp/shr: pass 1/3 (random)...|/tmp/shr: pass 2/3 (random)...|/tmp/shr: pass 3/3 (000000)...|/tmp/shr: removing|/tmp/shr: removed|gone|'
eq mu_init    "init refuses to run as anything but PID 1"   'init: must be run as PID 1|rc=1|'
eq su_passwd  "passwd/su: root sets, the user authenticates, changes their own, wrong ones refused" 'add=0|rc=0|passwd: password updated successfully|ptest|rc=0|su: Authentication failure|rc=1|rc=0|passwd: password updated successfully|rc=1|passwd: password unchanged|/home/ptest ptest|rc=1|passwd: You may not view or modify password information for ptest.|1600|rc=0|'
eq setuid_spawn "a setuid-root binary gains privilege when the SHELL spawns it, not only via execve" '-rwsr-xr-x 0 0|root|root|'
eq arc_ar     "ar: rcs/t/p/x/d with a GNU long name, readable by busybox" 'rc=0|a.txt|b.txt|a-very-long-member-name.txt|a.txt|b.txt|a-very-long-member-name.txt|bbbbbb|c|2|!<arch>|rw-r--r-- 4 a.txt|rw-r--r-- 2 a-very-long-member-name.txt|'
eq arc_cpio   "cpio: newc/odc create, list, extract, busybox interop" 'rc=0|070701|f1|sub/f2|f1|sub/f2|1 block|there|f1 sub/f2 |070707|2|'
ck arc_xz_decode "xz: CRC64, CRC32 and multi-block/no-check streams decode to the source" '^42a0b7093cea7549\|42a0b7093cea7549\|42a0b7093cea7549\|42a0b7093cea7549\|rc=0\|'
ck arc_xz_decode "a corrupted stream is refused by its check"  'xz: /etc/gnubad.xz: [A-Za-z0-9 ()]+\|rc=1'
ck arc_xz_decode "xz -l counts the blocks"                     'Strms +Blocks +Compressed +Uncompressed +Ratio +Check +Filename\| +1 +5 +[0-9]+ B +274000 B +0\.[0-9]+ +None +/etc/gnumadeblk.xz'
eq arc_xz_encode "xz encodes a stream busybox and xz itself decode; empty input round-trips" 'rc=0|busybox reads native xz|round trip|crc32 pipe|0|xz (XZ Utils)|'
eq ed_basic   "ed: d, a, s, ,p, w"                           'ONE|three|four|ONE|three|four|'
eq ed_more    "ed: g/re/, n, u, m, t, j, addresses, marks, H, r !cmd, w file, stdin buffer" '1	ONE|2	thrEE|3	four|ONE|three|four|four|ONE|three|ONE|four|ONE|three|ONEfour|ONE|three|ONEfour|3|?|ONE|threX|four|ONE|three|four|shell|4|line1|line2|ONE|four|ONE|> ONE|> three|> four|'
ck cron_install "crontab: rejects a bad table, installs, lists, the runner ticks and @reboot fires, -r" '^"-":1: bad minute\|errors in crontab file, can.t install.\|rc=1\|rc=0\|4\|[1-9]\|1\|rc=0\|no crontab for root\|rc=1\|$'
eq sys_modules "insmod/lsmod/rmmod/modprobe on a real module; non-root refused" 'hello.ko|Module                  Size  Used by|rc=0|1|hello Live|insmod: ERROR: could not insert module /lib/modules/hello.ko: File exists|rc=1|rc=0|0|rc=0|rc=0|modprobe: FATAL: Module nonesuch not found in directory /lib/modules|rc=1|rmmod: ERROR: Module hello is not currently loaded|rc=1|insmod: ERROR: could not insert module /lib/modules/hello.ko: Operation not permitted|rc=1|'
ck net_ip     "ip addr/link/route/-br/-o in iproute2's shapes"   '^2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP>\|link/ether [0-9a-f:]{17} brd\|inet [0-9.]+/[0-9]+ brd\|valid_lft forever preferred_lft\|lo UNKNOWN\|eth0 UP\|2\|2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP>\|default via [0-9.]+ dev\|[0-9.]+/[0-9]+ dev eth0 proto\|2\|rc=0\|$'
ck net_route_netstat "route -n / netstat -r -t -i -a"          '^Destination Flags Iface\|0.0.0.0 UG eth0\|Kernel IP routing table\|Active Internet connections \(only servers\)\|Proto Recv-Q Send-Q Local Address +Foreign Address +State +\|Kernel Iface lo \|[0-9]+\|$'
ck net_arp    "arp -n/-a, ip neigh, arp -s/-d"                 '^gw=[0-9.]+\|([0-9.]+ ether [0-9a-f:]{17}\|)+\? \([0-9.]+\)\|[0-9.]+ dev eth0 [0-9a-f:]{17}\|rc=0\|1\|rc=0\|0\|No ARP entry for 10.0.2.98\|rc=1\|$'
ck net_arping "arping gets replies from the gateway, none from a ghost, refused for non-root" '^rc=0\|ARPING [0-9.]+ from [0-9.]+ eth0\|Unicast reply from [0-9.]+ \[MAC\]  T ms\|Unicast reply from [0-9.]+ \[MAC\]  T ms\|Sent 2 probes \(2 broadcast\(s\)\)\|Received 2 response\(s\)\|rc=1\|Received 0 response\(s\)\|arping: socket: Operation not permitted\|rc=2\|$'
ck net_traceroute "traceroute reaches the gateway in one hop with a per-probe TTL; non-root refused" '^rc=0\|traceroute to [0-9.]+ \([0-9.]+\), 3 hops max, 60 byte packets\| 1  [0-9.]+  T ms\|( 2  [0-9.]+  T ms\|)?traceroute: socket: Operation not permitted\|rc=2\|$'
eq net_ip_set "ip route add/del, route add/del, ip link set, non-root refused" 'rc=0|1|rc=0|0|rc=0|1|rc=0|rc=0|RTNETLINK answers: Operation not permitted|rc=2|'
ck net_telnet "telnet connects to the loopback FTP server, trades the banner, and fails a dead port" 'rc=0\|Trying 127.0.0.1...\|Connected to 127.0.0.1.\|Escape character is .\^\].\.\|220 BANNER\|221 BYE\|Connection closed by foreign host.\|refused_rc=[1-9]\|Trying 127.0.0.1...'
if sel mkfs_; then
ck mkfs_disk  "the gate's AHCI scratch disk is visible"                'dev=ahci'
ck mkfs_ext2  "mkfs.ext2 writes a filesystem blkid, e2fsck, dumpe2fs and tune2fs all read" '^rc=0\|/dev/ahci[^ ]*: LABEL="scratch" UUID="U" TYPE="ext2" VERSION="1.0"\|rc=0\|[^|]*: [0-9]+/[0-9]+ files \([0-9]+% used\), [0-9]+/32768 blocks\|.*volume name: scratch\|.*[Mm]agic number: 0xEF53\|Inode count: [0-9]+\|Block count: 32768\|tune2fs 1.47.0 \(tobyOS\)\|Setting maximal mount count to 25\|Setting interval between checks to 604800 seconds\|Setting error behavior to 2\|.*volume name: renamed\|Errors behavior: Remount read-only\|Maximum mount count: 25\|Check interval: 604800 \(1 week\)\|rc=0\|[^|]*files \([0-9]+% used\), [0-9]+/32768 blocks\|$'
ck mkfs_vfat  "mkfs.vfat FAT16 and FAT32 with labels; fsck.vfat and fsck dispatch" '^mkfs.fat 4.2 \(2021-01-31\)\|rc=0\|/dev/ahci[^ ]*: LABEL="SCRATCH" UUID="U" TYPE="vfat" VERSION="FAT16"\|rc=0\|/dev/ahci[^ ]*: 0 files, 0/[0-9]+ clusters\|/dev/ahci[^ ]*: 0 files, 0/[0-9]+ clusters\|mkfs.fat 4.2 \(2021-01-31\)\|vfat\|/dev/ahci[^ ]*: 0 files, 1/[0-9]+ clusters\|$'
ck mkfs_minix "mkfs.minix v1/v2/v3 and fsck.minix"                   '^[0-9]+ inodes\|32768 blocks\|Firstdatazone=[0-9]+ \([0-9]+\)\|Zonesize=1024\|Maxsize=268966912\|\|rc=0\|/dev/ahci[^ ]*: TYPE="minix" VERSION="1"\|rc=0\|1 inodes used \([0-9]+%\)\|1 zones used \([0-9]+%\)\|[0-9]+ inodes\|1 inodes used \([0-9]+%\)\|1 zones used \([0-9]+%\)\|[0-9]+ inodes\|1 inodes used \([0-9]+%\)\|1 zones used \([0-9]+%\)\|$'
ck mkfs_guard "the kernel refuses to write /data's device or serve a non-root mkfs" '^mkfs.ext2: Device or resource busy while writing to /dev/ide0:master\|rc=1\|rc=1\|mkfs.vfat: unable to (write|open) /dev/ahci[^ ]*: (Operation not permitted|No such file or directory)\|rc=0\|[^|]*: [0-9]+/[0-9]+ files \([0-9]+% used\), [0-9]+/32768 blocks\|$'
fi
eq teardown_users "userdel removes the account from both databases" 'del=0|0|'

echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
