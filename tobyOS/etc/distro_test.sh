#!/bin/tsh
# The distro-parity tail: strip, dig/host, mkfs, domainname and cc.
#
# Driven from the boot harness (see coreutils_selftest in src/kernel.c), so
# the transcript lands on the serial log logs/distrotail.sh reads.

echo @@dt_BEGIN
cd /tmp

# --- strip --------------------------------------------------------------
# The proof is not that the file shrank -- truncation does that. It is that
# the SHRUNK BINARY STILL LOADS AND RUNS.
echo @@st_shrink
cp /bin/dig /tmp/dig
B=$(wc -c < /tmp/dig)
strip /tmp/dig
A=$(wc -c < /tmp/dig)
echo "shrank: $([ "$A" -lt "$B" ] && echo yes || echo no)"
echo @@st_run
/tmp/dig --version
echo @@st_debug_only
cp /bin/dig /tmp/dig2
strip -g /tmp/dig2
/tmp/dig2 --version
echo @@st_notelf
printf 'this is not an elf\n' > /tmp/ne.bin
strip /tmp/ne.bin 2>&1 | head -1
strip /tmp/ne.bin > /dev/null 2>&1; echo rc=$?
echo @@st_intact
cat /tmp/ne.bin

# --- dig / host ---------------------------------------------------------
echo @@dig_server
dig example.com 2>/dev/null | grep SERVER
echo @@dig_short
dig +short example.com 2>/dev/null | head -1
echo @@dig_answer
dig example.com 2>/dev/null | grep -c "ANSWER SECTION"
echo @@dig_nx
dig no-such-name-tobyos-test.invalid 2>/dev/null | grep "status:"
echo @@dig_mx
dig -t MX google.com 2>/dev/null | grep -c "IN	MX"
echo @@host_a
host -t A example.com 2>/dev/null | head -1
echo @@host_rev
host 8.8.8.8 2>/dev/null | head -1
echo @@host_nx
host -t A no-such-name-tobyos-test.invalid 2>&1 | head -1

# --- mkfs ---------------------------------------------------------------
echo @@mkfs_version
mkfs -V
echo @@mkfs_unknown
mkfs -t nosuchfs /tmp/img 2>&1 | head -1
mkfs -t nosuchfs /tmp/img > /dev/null 2>&1; echo rc=$?

# --- domainname ---------------------------------------------------------
echo @@dn_read
domainname
echo @@dn_set
domainname newdomain 2>&1 | head -1
domainname newdomain > /dev/null 2>&1; echo rc=$?

# --- cc -----------------------------------------------------------------
echo @@cc_same
echo "cc: $([ -f /bin/cc ] && echo present || echo missing)"
echo "same size as tcc: $([ "$(wc -c < /bin/cc)" = "$(wc -c < /bin/tcc)" ] && echo yes || echo no)"
echo @@cc_compile_void
printf 'int main(void){return 7;}\n' > /tmp/seven.c
cc -o /tmp/seven /tmp/seven.c 2>&1 | head -3
echo "built: $([ -f /tmp/seven ] && echo yes || echo no)"
echo @@cc_compile_empty
printf 'int main(){return 7;}\n' > /tmp/eight.c
cc -o /tmp/eight /tmp/eight.c 2>&1 | head -3
echo "built: $([ -f /tmp/eight ] && echo yes || echo no)"
echo @@cc_run
if [ -f /tmp/eight ]; then /tmp/eight; echo "ran rc=$?"; else echo "nothing to run"; fi

# --- at / batch / atq / atrm --------------------------------------------
# The runner is a detached child, so this is the one part that can only be
# tested where fork is real.
echo @@at_queue
echo 'echo hello-from-at' | at now + 1 hour 2>&1 | head -1
echo @@at_list
atq | head -1 | tr -s ' \t' ' '
echo @@at_badtime
echo x | at 'quarter past nine' 2>&1 | head -1
echo x | at 'quarter past nine' > /dev/null 2>&1; echo rc=$?
echo @@at_t
echo x | at -t 209901020304 2>&1 | head -1
echo @@at_empty
printf '' | at now + 1 day 2>&1 | head -1
echo @@at_hhmm
echo x | at 23:30 2>&1 | head -1
echo @@at_runs
rm -f /tmp/atran.txt
echo 'echo it-ran > /tmp/atran.txt' | at now > /dev/null 2>&1
N=0
while [ $N -lt 25 ]; do
    [ -f /tmp/atran.txt ] && break
    sleep 1
    N=$((N+1))
done
cat /tmp/atran.txt 2>/dev/null
echo "waited: $N"
echo @@at_cancel
rm -f /tmp/atcancel.txt
# The job id comes from at's OWN output. Taking it from `atq | tail -1`
# picked the LAST job by time -- atq sorts ascending, so that was the job
# scheduled for 2099, and atrm dutifully cancelled the wrong one.
OUT=$(echo 'echo should-not-run > /tmp/atcancel.txt' | at now + 5 sec 2>&1 | head -1)
echo "$OUT"
J=$(echo "$OUT" | awk '{print $2}')
atrm $J
sleep 9
echo "ran: $([ -f /tmp/atcancel.txt ] && echo yes || echo no)"
echo "still queued: $(atq | grep -c "^$J	")"
echo @@at_rmmissing
atrm 999 2>&1 | head -1
echo @@batch
echo 'echo batched' | batch 2>&1 | head -1

# --- setpci + /sys/.../config -------------------------------------------
# 00:03.0 is the e1000 in this QEMU machine; its vendor:device is 8086:100e.
echo @@pci_configfile
wc -c < /sys/bus/pci/devices/0000:00:03.0/config
echo @@setpci_vendor
setpci -s 00:03.0 VENDOR_ID
echo @@setpci_device
setpci -s 00:03.0 DEVICE_ID
echo @@setpci_class
setpci -s 00:03.0 CLASS_DEVICE
echo @@setpci_agrees
# The file and the syscall must report the same bytes -- two paths to one
# piece of hardware that disagree would be worse than having only one.
# `setpci --dump` renders the file's own bytes as hex, so the first line of
# the dump IS the first 16 bytes of the file, and comparing the two needs
# neither od's flags nor a printf that can emit a NUL.
setpci --dump -s 00:03.0 | head -1
head -c 16 /sys/bus/pci/devices/0000:00:03.0/config > /tmp/hdr16
echo "header bytes: $(wc -c < /tmp/hdr16)"
echo @@setpci_bydev
setpci -d 8086:100e VENDOR_ID
echo @@setpci_dump
setpci --dump -s 00:03.0 | head -1
echo @@setpci_write
# CACHE_LINE_SIZE, not COMMAND: clearing bus-master on the live NIC would
# take the network down for the rest of the boot.
ORIG=$(setpci -s 00:03.0 CACHE_LINE_SIZE)
echo "was: $ORIG"
setpci -s 00:03.0 CACHE_LINE_SIZE=20
echo "now: $(setpci -s 00:03.0 CACHE_LINE_SIZE)"
echo @@setpci_refresh
# The sysfs snapshot must move with the hardware. Checksummed rather than
# compared to expected bytes, so the check needs no NUL-writing printf.
A=$(head -c 16 /sys/bus/pci/devices/0000:00:03.0/config | cksum | cut -d' ' -f1)
setpci -s 00:03.0 CACHE_LINE_SIZE=$ORIG
B=$(head -c 16 /sys/bus/pci/devices/0000:00:03.0/config | cksum | cut -d' ' -f1)
setpci -s 00:03.0 CACHE_LINE_SIZE=20
C=$(head -c 16 /sys/bus/pci/devices/0000:00:03.0/config | cksum | cut -d' ' -f1)
echo "changed: $([ "$A" != "$B" ] && echo yes || echo no)"
echo "returned: $([ "$A" = "$C" ] && echo yes || echo no)"
setpci -s 00:03.0 CACHE_LINE_SIZE=$ORIG
echo @@setpci_badreg
setpci -s 00:03.0 NOSUCHREG 2>&1 | head -1
setpci -s 00:03.0 NOSUCHREG > /dev/null 2>&1; echo rc=$?
echo @@setpci_nowidth
setpci -s 00:03.0 0x10 2>&1 | head -1
echo @@setpci_nodev
setpci -s 1f:1e.7 VENDOR_ID 2>&1 | head -1
echo @@setpci_allwrite
setpci COMMAND=0x0007 2>&1 | head -1

# --- systemctl / service ------------------------------------------------
echo @@svc_list
systemctl list-units | head -1
echo @@svc_count
systemctl list-units | tail -1
echo @@svc_isactive
U=$(systemctl list-units | sed -n '2p' | awk '{print $1}' | sed 's/\.service$//')
echo "unit: $U"
systemctl is-active "$U"
echo @@svc_show
systemctl show "$U" | head -3
echo @@svc_refuse_start
systemctl start "$U" 2>&1 | head -1
systemctl start "$U" > /dev/null 2>&1; echo rc=$?
echo @@svc_refuse_enable
systemctl enable "$U" 2>&1 | head -1
echo @@svc_unknown
systemctl status nosuchunit 2>&1 | head -1
systemctl status nosuchunit > /dev/null 2>&1; echo rc=$?
echo @@service_all
service --status-all | head -1
echo @@svcctl_honest
svcctl start "$U" 2>&1 | grep -c "NOTHING CONSUMES"
svcctl start "$U" > /dev/null 2>&1; echo rc=$?

# --- getent -------------------------------------------------------------
echo @@getent_passwd
getent passwd root
echo @@getent_uid
getent passwd 0
echo @@getent_group
getent group wheel
echo @@getent_hosts
getent hosts localhost
echo @@getent_missing
getent passwd nosuchuser; echo rc=$?
echo @@getent_unknown
getent shadow 2>&1 | head -1
getent shadow > /dev/null 2>&1; echo rc=$?
echo @@getent_all
getent passwd | wc -l

# --- netcat -------------------------------------------------------------
echo @@netcat
echo "netcat: $([ -f /bin/netcat ] && echo present || echo missing)"
echo "same as nc: $([ "$(wc -c < /bin/netcat)" = "$(wc -c < /bin/nc)" ] && echo yes || echo no)"

# --- compress / uncompress (.Z) -----------------------------------------
# `uncompress` was a second name for gunzip, which cannot read a .Z at all.
echo @@z_roundtrip
cd /tmp && rm -f zt.txt zt.txt.Z
cp /tmp/zx/gnumade.txt zt.txt 2>/dev/null || cp /etc/motd zt.txt
B=$(wc -c < zt.txt)
compress zt.txt
echo "made: $([ -f /tmp/zt.txt.Z ] && echo yes || echo no)"
echo "smaller: $([ "$(wc -c < /tmp/zt.txt.Z)" -lt "$B" ] && echo yes || echo no)"
uncompress zt.txt.Z
echo "same size: $([ "$(wc -c < /tmp/zt.txt)" = "$B" ] && echo yes || echo no)"
echo @@z_magic
compress -c zt.txt > /tmp/zt2.Z
head -c 2 /tmp/zt2.Z | cksum | cut -d' ' -f1
echo @@z_zcat
# zcat recognises .Z by its magic, the way GNU gzip does
zcat /tmp/zt2.Z | wc -c
echo @@z_notz
uncompress -c /etc/motd 2>&1 | head -1
uncompress -c /etc/motd > /dev/null 2>&1; echo rc=$?

# --- nc: TCP by default -------------------------------------------------
# sshd is listening on 22 in this harness, which is a real TCP server to
# connect to. `nc` used to be UDP-only, so this could not have passed.
echo @@nc_tcp_open
# ONE connection to sshd, not three. This server handles a single session
# at a time, and repeatedly probing it is the documented way to wedge it --
# which would then fail logs/sshclient.sh in the same boot, for a reason
# that has nothing to do with ssh.
OUT=$(nc -z -w 2 127.0.0.1 22 2>&1); RC=$?
echo "$OUT" | head -1
echo rc=$RC
echo @@nc_tcp_closed
# Port 9 has no listener, and nothing else in the boot cares about it.
OUT=$(nc -z -w 2 127.0.0.1 9 2>&1); RC=$?
echo "$OUT" | head -1
echo rc=$RC

# --- raw sockets: ping, tcpdump, clockdiff -------------------------------
# 10.0.2.2 is QEMU's gateway and answers ICMP echo. None of these three
# tools could exist before the kernel had SOCK_RAW.
echo @@ping_gateway
ping -c 2 -W 3 10.0.2.2
echo @@ping_stats
ping -c 2 -W 3 -q 10.0.2.2 | tail -2
echo @@ping_badhost
ping -c 1 no-such-host-here 2>&1 | head -1
ping -c 1 no-such-host-here > /dev/null 2>&1; echo rc=$?
echo @@tcpdump_capture
# -c 2, and enough ping traffic to satisfy it well before the wait ends:
# asking for 4 frames and looking after 3 seconds read the file while
# tcpdump was still waiting for the fourth, so the summary was not there yet.
tcpdump -c 2 > /tmp/dump.txt 2>&1 &
sleep 1
ping -c 4 -q -W 2 10.0.2.2 > /dev/null 2>&1
sleep 4
echo "frames: $(grep -c 'IP \|ARP' /tmp/dump.txt)"
echo "summary: $(grep -c 'packets captured' /tmp/dump.txt)"
echo @@clockdiff
# Most hosts ignore ICMP TIMESTAMP; whichever way it goes must be SAID.
clockdiff 10.0.2.2 2>&1 | head -1

# --- ftp, against the ftpd running in this same boot ---------------------
echo @@ftp_setup
rm -rf /tmp/ftproot /tmp/ftpget
mkdir /tmp/ftproot
mkdir /tmp/ftpget
printf 'hello from the ftp server\n' > /tmp/ftproot/greeting.txt
# A pristine copy OUTSIDE the served root: the session deletes greeting.txt
# from the server on purpose, so comparing the download against the server's
# copy afterwards compares it against a file that is meant to be gone.
cp /tmp/ftproot/greeting.txt /tmp/greeting.orig
cp /etc/gnumade.txt /tmp/ftproot/big.txt 2>/dev/null || cp /etc/motd /tmp/ftproot/big.txt
printf 'payload to upload\n' > /tmp/ftpget/upload.txt
echo "served: $(ls /tmp/ftproot | wc -l) files"
ftpd -n 1 -d /tmp/ftproot > /tmp/ftpd.log 2>&1 &
sleep 2
echo @@ftp_session
cd /tmp/ftpget
printf 'user anonymous\npwd\nls\nsize greeting.txt\nget greeting.txt\n' > /tmp/ftpcmd
printf 'put upload.txt\nmkdir subdir\nls\nrmdir subdir\n' >> /tmp/ftpcmd
printf 'delete greeting.txt\nls\nquit\n' >> /tmp/ftpcmd
ftp -v 127.0.0.1 21 < /tmp/ftpcmd
echo @@ftp_serverlog
cat /tmp/ftpd.log 2>/dev/null | head -8
echo "ftpd still running: $(pgrep ftpd | wc -l)"
echo @@ftp_bytes
echo "same: $(cmp -s /tmp/greeting.orig /tmp/ftpget/greeting.txt && echo yes || echo no)"
echo @@ftp_upload
echo "uploaded: $([ -f /tmp/ftproot/upload.txt ] && echo yes || echo no)"
echo "identical: $(cmp -s /tmp/ftpget/upload.txt /tmp/ftproot/upload.txt && echo yes || echo no)"
echo @@ftp_deleted
echo "greeting still there: $([ -f /tmp/ftproot/greeting.txt ] && echo yes || echo no)"
echo @@ftp_noserver
# Nothing listens on 2121; the client must say so rather than hang.
ftp 127.0.0.1 2121 < /dev/null 2>&1 | head -1
cd /tmp

# --- sudo ---------------------------------------------------------------
# /bin/sudo is staged mode 4755 by a tar pass of its own; the main pass
# forces 0755 over everything and the setuid bit would not survive it.
echo @@sudo_mode
ls -l /bin/sudo | cut -c1-10
echo @@sudo_root
sudo id | head -1
sudo -n true; echo rc=$?
echo @@sudo_list
sudo -l | head -2
echo @@sudo_nocmd
sudo definitely-not-a-command 2>&1 | head -1
sudo definitely-not-a-command > /dev/null 2>&1; echo rc=$?
echo @@sudo_nouser
sudo -u no-such-user-at-all id 2>&1 | head -1
echo @@sudo_sudoers
grep -c wheel /etc/sudoers

# --- ranlib -------------------------------------------------------------
# /etc/libtest.a is a real ar archive of two cross-compiled objects,
# committed WITHOUT an index so ranlib has something to do.
echo @@ranlib_list
cp /etc/libtest.a /tmp/lib.a
ranlib -t /tmp/lib.a
echo @@ranlib_write
ranlib -v /tmp/lib.a
echo "grew: $([ "$(wc -c < /tmp/lib.a)" -gt "$(wc -c < /etc/libtest.a)" ] && echo yes || echo no)"
echo @@ranlib_idempotent
cp /tmp/lib.a /tmp/lib1.a
ranlib /tmp/lib.a
echo "stable: $(cmp -s /tmp/lib.a /tmp/lib1.a && echo yes || echo no)"
echo @@ranlib_notar
printf 'not an archive\n' > /tmp/bogus.a
ranlib /tmp/bogus.a 2>&1 | head -1
ranlib /tmp/bogus.a > /dev/null 2>&1; echo rc=$?

# --- addr2line ----------------------------------------------------------
# /etc/dwarf_test.elf carries real DWARF 4; the expected answers were taken
# from GNU addr2line on the host before this was written down.
echo @@a2l_line
addr2line -e /etc/dwarf_test.elf -s 201120
echo @@a2l_func
addr2line -e /etc/dwarf_test.elf -f -s 201120
echo @@a2l_unmapped
addr2line -e /etc/dwarf_test.elf ffffffff | tail -1
echo @@a2l_nodebug
addr2line -e /bin/echo 400000 2>&1 | head -1

# --- gdb: real breakpoints over ptrace ----------------------------------
# /etc/dbg_test.elf calls dbg_step three times and then exits 7. The
# breakpoint is a real int3 the debugger writes into the target's text.
echo @@gdb_session
printf 'break dbg_step\nrun\ninfo registers\ncontinue\ncontinue\ndelete\ncontinue\nquit\n' > /tmp/gdbcmd
gdb /etc/dbg_test.elf < /tmp/gdbcmd 2>&1 | head -60
echo @@gdb_exitcode
# gdb's "[Inferior 1 (process N) exited with code 7]" begins with '[', which
# the gate's kernel-line filter drops from every section -- so the line is
# counted HERE, where only the count crosses the filter.
gdb /etc/dbg_test.elf < /tmp/gdbcmd 2>&1 | grep -c "exited with code 7"
echo @@gdb_fixture
# Does the target exit on its own, with no debugger anywhere near it? This
# separates "the debugger cannot resume" from "the fixture never exits".
# BOUNDED. The first version of this fixture never exited, and running it
# unbounded here hung the whole boot -- a broken fixture must fail a check,
# not take the machine down with it.
timeout 5 /etc/dbg_test.elf > /dev/null 2>&1; echo "standalone rc=$?"
echo @@gdb_nosym
printf 'break no_such_symbol\nquit\n' > /tmp/gdbcmd2
gdb /etc/dbg_test.elf < /tmp/gdbcmd2 2>&1 | grep -i "not defined" | head -1

# --- exec really replaces the process -----------------------------------
# `env CMD` execs CMD. libtoby used to SPAWN it and wait, so an `env`
# process stayed alive alongside the command; a real exec leaves only the
# command. Counting the processes is what tells the two apart.
echo @@exec_replaces
env sleep 20 &
sleep 2
echo "env wrappers: $(pgrep env | wc -l)"
echo "sleep running: $(pgrep sleep | wc -l)"
killall sleep 2>/dev/null

# --- strace: real syscall tracing ---------------------------------------
# /etc/dbg_test.elf is a NATIVE tobyOS program, so this exercises the
# native-path syscall stops (the Linux path has always had them). Bounded,
# because every traced syscall is two context switches.
# `busybox arch` runs under the LINUX personality (/bin/arch is native
# since 2026-09-04) --
# which is where the kernel's syscall stops live. Bounded, because every
# traced syscall is two context switches.
echo @@strace_run
timeout 40 strace busybox arch 2>&1 | head -8
echo @@strace_exit
timeout 40 strace busybox arch 2>&1 | grep "exited with" | head -1
echo @@strace_count
timeout 40 strace -c busybox arch 2>&1 | head -16
echo @@strace_native
# A NATIVE tobyOS program, traced through the native dispatcher's stops.
# Enabled 2026-09-01, wedged real hardware, reverted; the real cause was the
# shared ptrace_stop() yielding under the BKL (3c523d2), which the Linux
# path had too. The fixture exits 7, and that exit is a native syscall.
timeout 40 strace /etc/dbg_test.elf 2>&1 | tail -5
echo @@strace_native_repeat
# Ten in a row. The second wedge mechanism (a tracer exiting with its tracee
# parked forever) was cumulative, and a single trace could never show it.
n=0; i=0
while [ $i -lt 10 ]; do
    i=$((i+1))
    timeout 40 strace /etc/dbg_test.elf 2>&1 | grep -q "exited with 7" && n=$((n+1))
done
echo "traced: $n/10"
ps -e -o comm= | grep -c dbg_test

# --- #! scripts ----------------------------------------------------------
# Neither loader read a shebang: `./script.sh` returned 127 from a shell
# that could run every binary on the system and none of the scripts. Both
# (spawn and execve) now hand a "#!" file to its interpreter by Linux's
# rules: one optional argument, the script's path in place of argv[0],
# nesting bounded at 4. A text file with NO "#!" is bash's to run itself.
echo @@shebang_spawn
printf '#!/bin/tsh\necho "shebang ok $1 $#"\n' > /tmp/sb1.sh; chmod +x /tmp/sb1.sh
/tmp/sb1.sh world; echo rc=$?
echo @@shebang_exec
# `env` execs its command: this is the execve loader, not spawn.
env /tmp/sb1.sh via-execve; echo rc=$?
echo @@shebang_optarg
# Everything after the interpreter is ONE argument, then the script's path,
# then the script's own arguments -- /bin/echo prints its argv, so this is
# the interpreter's argv verbatim.
printf '#!/bin/echo hello there\n' > /tmp/sb2.sh; chmod +x /tmp/sb2.sh
/tmp/sb2.sh a b
echo @@shebang_nested
printf '#!/tmp/sb2.sh\n' > /tmp/sb3.sh; chmod +x /tmp/sb3.sh
/tmp/sb3.sh z
echo @@shebang_busybox
# A Linux-personality interpreter, through the same loader.
printf '#!/bin/busybox sh\necho "busybox sh: $1"\n' > /tmp/sb6.sh; chmod +x /tmp/sb6.sh
/tmp/sb6.sh ok; echo rc=$?
echo @@shebang_badinterp
printf '#!/no/such/interp\n' > /tmp/sb4.sh; chmod +x /tmp/sb4.sh
/tmp/sb4.sh 2>&1; echo rc=$?
echo @@shebang_loop
# A script that names itself as its interpreter: bounded, not recursed.
printf '#!/tmp/sb5.sh\n' > /tmp/sb5.sh; chmod +x /tmp/sb5.sh
/tmp/sb5.sh 2>&1; echo rc=$?
echo @@shebang_plain
printf 'echo "plain text $1 $0"\n' > /tmp/sb7.sh; chmod +x /tmp/sb7.sh
/tmp/sb7.sh yes; echo rc=$?
echo @@shebang_plain_pipe
/tmp/sb7.sh piped | cat; echo rc=$?
echo @@shebang_plain_linux
# busybox sh runs a #!-less text file itself too -- given ENOEXEC from the
# loader, which used to answer with a code that read as "no such file".
busybox sh -c '/tmp/sb7.sh bb'; echo rc=$?
echo @@shebang_plain_execvp
# and libtoby's execvp does what POSIX says: through the shell.
env /tmp/sb7.sh execvp; echo rc=$?
echo @@shebang_binary
printf 'binary\000junk\000' > /tmp/sb8; chmod +x /tmp/sb8
/tmp/sb8 2>&1; echo rc=$?
echo @@shebang_noexec
printf 'echo never\n' > /tmp/sb9.sh; chmod 644 /tmp/sb9.sh
/tmp/sb9.sh 2>&1; echo rc=$?

# --- /proc/stat: real accounting ------------------------------------------
# The cpu, intr and ctxt counters were literal zeros ("nothing accumulates
# them"), and /proc/loadavg was "0.00 0.00 0.00". Now the 100 Hz tick, the
# ISR dispatcher and the scheduler's switch do the accumulating. The proof
# is a DELTA across real work, not a nonzero number.
echo @@pstat_before
grep -E '^(cpu |intr |ctxt |btime |processes |procs_running )' /proc/stat | cut -c1-72
echo @@pstat_work
i=0; while [ $i -lt 40000 ]; do i=$((i+1)); done; echo "looped $i"
echo @@pstat_after
grep -E '^(cpu |intr |ctxt |btime |processes |procs_running )' /proc/stat | cut -c1-72
echo @@pstat_loadavg
cat /proc/loadavg
echo @@pstat_vmstat
vmstat 1 2 2>/tmp/vmstat.err; echo "stderr: $(cat /tmp/vmstat.err)"

# --- chmod: symbolic modes ------------------------------------------------
# /bin/chmod accepted three octal digits and nothing else, so `chmod +x`
# printed "invalid mode" -- which is how every script fixture above stayed
# 0644 on the first run of this gate. busybox's stat reads the bits back.
echo @@chmod_sym
printf 'x\n' > /tmp/cm1; chmod 644 /tmp/cm1
chmod +x /tmp/cm1;          stat -c '%a' /tmp/cm1
chmod u+s,g-x /tmp/cm1;     stat -c '%a' /tmp/cm1
chmod a=r /tmp/cm1;         stat -c '%a' /tmp/cm1
chmod -w /tmp/cm1;          stat -c '%a' /tmp/cm1
chmod u=rwx,go=rx /tmp/cm1; stat -c '%a' /tmp/cm1
chmod o+t /tmp/cm1;         stat -c '%a' /tmp/cm1
chmod 0600 /tmp/cm1;        stat -c '%a' /tmp/cm1
echo @@chmod_verbose
chmod -v 644 /tmp/cm1
chmod -c 644 /tmp/cm1; echo "rc=$?"
chmod -v 755 /tmp/cm1
echo @@chmod_recursive
mkdir -p /tmp/cmd/sub; printf 'y\n' > /tmp/cmd/sub/f; chmod 600 /tmp/cmd/sub/f
chmod -R g+r /tmp/cmd; stat -c '%a' /tmp/cmd/sub/f
# X: execute for a directory, or for a file that already has an x bit.
chmod -R 700 /tmp/cmd; chmod 600 /tmp/cmd/sub/f; chmod -R +X /tmp/cmd
stat -c '%a' /tmp/cmd/sub /tmp/cmd/sub/f
echo @@chmod_bad
chmod +q /tmp/cm1 2>&1; echo "rc=$?"
chmod 644 /tmp/nonexistent 2>&1; echo "rc=$?"

# --- printf: a NUL is a byte ---------------------------------------------
# `printf '%s\0'` is how every find|xargs -0 pipeline is fed. The shell's
# output sink took C strings, so the \0 was silently nothing -- which is
# also why the "binary" fixture above was plain text on the first run.
echo @@printf_nul
printf '%s\0' x y | wc -c
printf 'a\0b' | wc -c
printf 'a\0b\n' | od -An -c | tr -s ' '

# --- the native clock is the real clock -----------------------------------
# /bin/date is native, and it said 2025 on a box whose RTC said 2026:
# libtoby's time_t was seconds since boot on a pretend 2025-01-01 base,
# because the native ABI had no realtime syscall to ask. busybox runs under
# the Linux personality and reads the kernel's RTC-anchored clock, so it is
# the oracle here: the two personalities must agree about what time it is,
# and about how every strftime conversion spells it.
echo @@clock_agree
N=$(date +%s); B=$(busybox date +%s)
D=$((B - N)); [ "$D" -lt 0 ] && D=$((0 - D))
echo "native=$N busybox=$B skew=$D"
echo @@clock_year
date +%Y
busybox date +%Y
echo @@clock_default
date
echo @@clock_fixed
# A fixed instant, so the two outputs can be compared byte for byte.
F='%Y-%m-%d|%H:%M:%S|%a %A|%b %B|%e|%j|%u %w|%U %W|%V %G %g|%C %y|%D|%F|%T|%R|%r|%I %p|%Z|%z|%s|%%'
date -u -d @1700000000 "+$F"
busybox date -u -d @1700000000 "+$F"
echo @@clock_fixed_flags
date -u -d @1700000000 '+%-d|%-m|%_H|%-H|%010Y|%c'
busybox date -u -d @1700000000 '+%-d|%-m|%_H|%-H|%010Y|%c'
echo @@clock_gnu_only
# GNU conversions busybox's libc does not know (its output STOPS at %k):
date -u -d @1700000000 '+%k|%l|%P|%q'
echo @@clock_dateparse
date -u -d '2024-02-29 12:34:56' +%s
date -u -d '2024-02-29T12:34' +%s
date -u -d 2024-02-29 +%s
date -u -d @0
date -d 'nonsense' 2>&1; echo rc=$?
echo @@clock_iso_rfc
date -u -d @1700000000 -I
date -u -d @1700000000 -Iseconds
date -u -d @1700000000 -R
echo @@clock_mtime
# A file created by a NATIVE program (touch), stamped by the filesystem,
# read back by busybox stat -- and the reverse, native date -r and ls -l.
rm -f /tmp/ck1 /tmp/ck2; touch /tmp/ck1
M=$(busybox stat -c %Y /tmp/ck1); N=$(date +%s)
D=$((N - M)); [ "$D" -lt 0 ] && D=$((0 - D))
echo "mtime=$M now=$N skew=$D"
date -r /tmp/ck1 +%Y
busybox touch /tmp/ck2; ls -l /tmp/ck2 | awk '{print $6}'
echo @@clock_printf_T
printf '%(%Y)T\n' -1
printf '%(%Y)T\n'
T=$(printf '%(%s)T' -1); N=$(date +%s); D=$((N - T)); [ "$D" -lt 0 ] && D=$((0 - D)); echo "skew=$D"
printf '%(%Y-%m-%d %H:%M:%S %A %j)T\n' 1700000000
printf '%(%s)T\n' 1700000000
# a leading x: the gate drops any line that begins with '[' as a kernel line
printf 'x[%12(%F)T][%-12(%F)T]\n' 86400 86400
printf '%()T\n' 0

# --- ps: processes, not services ------------------------------------------
# /bin/ps listed the SERVICES the supervisor manages, and the shell builtin
# listed processes. On Linux `ps` lists processes, from /proc, which this
# kernel had been serving to busybox ps all along. The service listing moved
# to `svcctl list`. The oracles: /proc itself, busybox ps, and procps's
# column headers verbatim.
echo @@ps_self
ps -p $$ -o pid=,comm=
ps -o comm= -p $$
echo "dollarpipe: $(echo $$ | cat) main $$"
echo @@ps_header_bare
ps | head -1
echo @@ps_header_aux
ps aux | head -1
echo @@ps_header_ef
ps -ef | head -1
echo @@ps_header_ax
ps ax | head -1
echo @@ps_aux_tsh
ps aux | grep -c '[t]sh'
echo @@ps_pidset
# Two snapshots a few milliseconds apart: the only pids that may differ are
# the ones the snapshot commands themselves are.
ps -e -o pid= | tr -d ' ' | sort -n > /tmp/ps_e.txt
echo "count: $(grep -c . /tmp/ps_e.txt)"
# The shell (pid $$) must be in `ps -e`. pid 0 is deliberately NOT here:
# Linux `ps -e` omits the scheduler (pid 0) and busybox ps agrees.
echo "shell: $(grep -cx "$$" /tmp/ps_e.txt)"
echo @@ps_busybox
busybox ps -o pid= | tr -d ' ' | sort -n > /tmp/ps_bb.txt
echo "only-in-one: $(sort /tmp/ps_e.txt /tmp/ps_bb.txt | uniq -u | grep -c .)"
echo @@ps_columns
ps -o pid,ppid,pgid,user,%cpu,%mem,vsz,rss,stat,etime,time,comm -p $$
echo @@ps_custom_header
ps -o pid=PROCESS,comm=NAME -p $$
ps -o comm= -p $$
echo @@ps_etime
sleep 3 &
SP=$!
sleep 2
ps -o etimes= -p $SP | tr -d ' '
ps -o stat= -p $SP | cut -c1
ps -o ppid= -p $SP | tr -d ' '
echo "self=$$"
kill $SP 2>/dev/null; wait $SP 2>/dev/null
echo @@ps_none
ps -p 99999 > /dev/null; echo rc=$?
ps -C nosuchcommand -o pid= | grep -c .
ps -o nosuchcolumn 2>&1 | head -1
echo @@ps_services
ps --json > /dev/null 2>&1; echo rc=$?
svcctl list | grep -c .

# --- /bin/sh is tsh ---------------------------------------------------------
# /bin/sh was programs/user_sh, a minimal shell that scored 0/14 against
# bash -- and system(), popen(), execvp's ENOEXEC fallback and every
# #!/bin/sh script went through it. It is a link to /bin/tsh now, in POSIX
# mode, the way /bin/sh is bash on many a distro.
echo @@sh_same_binary
echo "same as tsh: $([ "$(wc -c < /bin/sh)" = "$(wc -c < /bin/tsh)" ] && echo yes || echo no)"
echo @@sh_c
sh -c 'echo $((1+2)); x=abc; echo ${x#a} ${x%c} ${#x}; for i in 1 2; do printf "%s-" $i; done; echo'
echo @@sh_posix_mode
sh -c 'set -o | grep posix'
tsh -c 'set -o | grep posix'
echo @@sh_script
cat > /tmp/posix.sh <<'EOF'
#!/bin/sh
f() { return $(( $1 * 2 )); }
f 21; r=$?
case "$r" in 42) echo "case ok" ;; *) echo "case bad $r" ;; esac
cat <<EOT
heredoc $r
EOT
[ -n "${HOME:-}" ] && echo "test ok"
echo "$0 $#"
EOF
chmod +x /tmp/posix.sh
/tmp/posix.sh a b; echo rc=$?
echo @@sh_system
# /bin/systest is a NATIVE C program (built with libtoby, in the image) whose
# system() and popen() must reach a real shell: a pipeline, an exit status
# carried through waitpid's encoding, popen for reading AND writing, and a
# quoted argument. This used to route through the 0/14 minimal shell.
systest

# =========================================================================
# Native-parity batch (2026-09-04): the 46 busybox-only utilities, natively.
# Section prefixes: census_ setup_ proc_ tsh_ mu_ (coreutils) su_ arc_
# (archive) ed_ cron_ sys_ (modules) net_ mkfs_ (needs the gate's AHCI
# scratch disk: QEMU only, excluded from hwgate's default set) teardown_.
# The native awk is a 313-line subset ($NF, ?:, sub() and `$1=="x"` patterns
# are not there): these sections use sed/cut/grep. `tail` reads files, never
# pipes. Privilege tests run as `pcall`, created in setup_users.
# =========================================================================
echo @@census_46
# every one of the 46 must be a real /bin file that is NOT the busybox binary
for n in arch chgrp factor false link logname nproc printf pwd shred sum test true unlink uptime users who ar cpio unxz xz xzcat ed crontab init insmod lsmod modprobe rmmod blkid fsck mkfs.ext2 mkfs.minix mkfs.vfat tune2fs arp arping hostid ip netstat route telnet traceroute passwd su; do
    if [ ! -f "/bin/$n" ]; then echo "MISSING $n"
    elif cmp -s "/bin/$n" /bin/busybox; then echo "BUSYBOX $n"; fi
done
echo "census done"

echo @@setup_users
# useradd registers in BOTH /etc/passwd and the kernel's login database now
userdel pcall > /dev/null 2>&1
useradd -u 1601 -g 100 -s /bin/tsh pcall > /tmp/ua.out 2>&1; echo add=$?
grep -c '^pcall:' /etc/passwd
grep -c 'kernel login database' /tmp/ua.out
sudo -u pcall id -u

echo @@proc_new
cut -d' ' -f1 /proc/vmstat | tr '\n' ' '; echo
PF=$(sed -n 's/^pgfault //p' /proc/vmstat); [ "${PF:-0}" -gt 0 ] && echo "faults counted" || echo "no faults"
head -1 /proc/net/route | tr '\t' ' ' | tr -s ' ' | cut -c1-40
grep -c eth0 /proc/net/route
head -1 /proc/net/arp | tr -s ' ' | cut -c1-30

echo @@tsh_caps
# ARG_MAX 32 -> 512 (hosted); $(...) capture 1 KiB -> 8 KiB (hosted); "" -> 127
tsh -c 'set -- $(seq 1 255); echo $#'
tsh -c 'x=$(seq 512 | tr "\n" " "); echo ${#x}'
tsh -c '"" -c x' 2>&1; echo rc=$?
tsh -c 'for i in $(seq 1 200); do :; done; echo loop ok'

echo @@mu_arch
/bin/arch; busybox arch
echo @@mu_true_false
/bin/true; echo rc=$?; /bin/false; echo rc=$?; /bin/true --version | head -1 | cut -d' ' -f1
echo @@mu_printf
/bin/printf '%s|%5d|%-4s|%03o|%x|%c|%b|%%\n' abc 42 ab 8 255 zed 'x\ty'
/bin/printf '%d %d\n' 1 2 3
/bin/printf '%.3f %e %g %+d %.2f\n' 3.14159 1234.5 0.0001 7 2.006
/bin/printf '\x41\102\u00e9\n'
/bin/printf '%q\n' "it's"
/bin/printf '%d\n' abc 2>&1; echo rc=$?
/bin/printf '%5.2s|%-6b|\n' abcdef 'a\nb'
echo @@mu_test
/bin/test 3 -gt 2; echo rc=$?
/bin/test abc = abd; echo rc=$?
/bin/[ -d /tmp -a -f /bin/tsh ]; echo rc=$?
/bin/test ! \( 1 -eq 1 -o 2 -eq 3 \); echo rc=$?
env /bin/[ x > /tmp/bt.out 2>&1; RC=$?; grep 'missing' /tmp/bt.out | sed 's/^/x/'; echo rc=$RC
/bin/test -n "" -o -z ""; echo rc=$?
/bin/test 1 -eq x 2>&1; echo rc=$?
/bin/test -e /bin/tsh -a ! -e /nonesuch; echo rc=$?
/bin/test /bin/tsh -ef /bin/tsh; echo rc=$?
echo @@mu_factor
/bin/factor 360 97 1 1000000007
echo 12 | /bin/factor
/bin/factor abc 2>&1; echo rc=$?
echo @@mu_sum
printf 'hello world\n' > /tmp/sum.txt
/bin/sum /tmp/sum.txt; busybox sum /tmp/sum.txt
/bin/sum -s /tmp/sum.txt; busybox sum -s /tmp/sum.txt
echo @@mu_link_unlink
# a hard link shares the inode: write through one name, read through the other
D=/tmp; [ -w /data ] && mkdir -p /data/nptest && D=/data/nptest
echo one > $D/lk1; rm -f $D/lk2
/bin/link $D/lk1 $D/lk2 2>&1; echo rc=$?
[ -f $D/lk2 ] && echo two > $D/lk2 && cat $D/lk1
/bin/unlink $D/lk2 2>&1; echo rc=$?
[ -e $D/lk2 ] && echo still || echo gone
cat $D/lk1
/bin/unlink $D/nonesuch 2>&1; echo rc=$?
rm -f $D/lk1; [ "$D" = /data/nptest ] && rmdir /data/nptest
echo @@mu_inode_identity
# INODE IDENTITY (2026-09-08). Every one of these was wrong before struct
# stat carried st_nlink/st_ino/st_dev: `ls -l` printed a synthetic link
# count of 1 however many names a file had, and `test -ef` compared
# canonicalised PATHS, so two hard links to one file -- the only thing the
# operator exists to detect -- compared as different files.
D=/tmp; [ -w /data ] && mkdir -p /data/idtest && D=/data/idtest
rm -f $D/id1 $D/id2 $D/id3
echo body > $D/id1
echo body > $D/id3
# tmpfs cannot make a hard link (node-per-path); say so rather than assert
# an outcome that depends on which filesystem $D landed on.
if /bin/link $D/id1 $D/id2 2>/dev/null; then echo "link=ok"; else echo "link=unsupported"; fi
[ $D/id1 -ef $D/id2 ] && echo "same=yes" || echo "same=no"
[ $D/id1 -ef $D/id3 ] && echo "other=yes" || echo "other=no"
[ $D/id1 -ef $D/id1 ] && echo "self=yes" || echo "self=no"
# the link count is REAL: two names, then one again after unlink
/bin/ls -l $D/id1 | awk '{print "nlink=" $2}'
/bin/unlink $D/id2 2>/dev/null
/bin/ls -l $D/id1 | awk '{print "after=" $2}'
/bin/ls -ld $D | awk '{print "dir>=2:" ($2 >= 2 ? "yes" : "no")}'
rm -f $D/id1 $D/id3; [ "$D" = /data/idtest ] && rmdir /data/idtest
echo @@mu_logname_users
/bin/logname; echo rc=$?
echo "users=[$(/bin/users)]"
echo "who=$(/bin/who | wc -l)"
/bin/who -q | grep 'users='
/bin/who -b | grep -c 'system boot'
/bin/nproc; /bin/nproc --ignore=1
echo "hostid=$(/bin/hostid)"
echo @@mu_uptime
/bin/uptime
/bin/uptime -p
/bin/uptime -s
echo @@mu_pwd
cd /tmp && /bin/pwd && /bin/pwd -P && cd /
echo @@mu_chgrp
touch /tmp/cg; /bin/chgrp -v wheel /tmp/cg; ls -l /tmp/cg | awk '{print $4}'
/bin/chgrp 20 /tmp/cg; ls -ln /tmp/cg | awk '{print $4}'
/bin/chgrp nogroup /tmp/cg 2>&1; echo rc=$?
echo @@mu_shred
printf 'secret data here\n' > /tmp/shr
/bin/shred -n 2 -z -u -v /tmp/shr 2>&1 | sed 's/^shred: //'
[ -e /tmp/shr ] && echo still || echo gone
echo @@mu_init
/bin/init 2>&1; echo rc=$?

echo @@su_passwd
userdel ptest > /dev/null 2>&1
useradd -u 1600 -g 100 -s /bin/tsh ptest > /dev/null 2>&1; echo add=$?
mkdir -p /home/ptest
printf 'pw-one\npw-one\n' | /bin/passwd ptest > /tmp/pw1.out 2>&1; echo rc=$?; grep '^passwd:' /tmp/pw1.out
# a non-root caller must present the target's password
sudo -u pcall /bin/su ptest -c 'id -un' <<'EOF'
pw-one
EOF
echo rc=$?
sudo -u pcall /bin/su ptest -c 'id -un' <<'EOF'
wrong
EOF
echo rc=$?
# the user changes their own password: the current one is checked by the KERNEL
sudo -u ptest /bin/passwd > /tmp/pw2.out 2>&1 <<'EOF'
pw-one
pw-two
pw-two
EOF
echo rc=$?; grep '^passwd: password' /tmp/pw2.out
sudo -u ptest /bin/passwd > /tmp/pw3.out 2>&1 <<'EOF'
nope
pw-three
pw-three
EOF
echo rc=$?; grep '^passwd: password' /tmp/pw3.out
sudo -u pcall /bin/su - ptest -c 'echo $HOME $USER' <<'EOF'
pw-two
EOF
# a user may not set someone else's password
sudo -u pcall /bin/passwd ptest > /tmp/pw4.out 2>&1 <<'EOF'
x
y
y
EOF
echo rc=$?; grep '^passwd:' /tmp/pw4.out
/bin/su ptest -c 'id -u'; echo rc=$?
userdel ptest > /dev/null 2>&1

echo @@setuid_spawn
# The set-user-ID bit must be applied by the SHELL's spawn path, not only by
# execve. It was execve-only until 2026-09-06, so `sudo` worked under
# `busybox sh -c` (fork+execve) and was dead from tsh -- found on hardware,
# invisible here because every gate runs as ROOT and root never needs the bit.
# A setuid-root copy of logname needs no password: it prints the name for the
# EFFECTIVE uid, so it says "root" exactly when the transition happened.
# the copy MUST keep the name `logname`: user_moreutils is a multicall
# binary and dispatches on basename(argv[0]).
mkdir -p /tmp/sid
cp /bin/logname /tmp/sid/logname
chown 0:0 /tmp/sid/logname 2>/dev/null
chmod 4755 /tmp/sid/logname
# mode AND numeric owner: the transition sets the effective uid to the FILE
# OWNER, so a copy owned by anyone but root proves nothing.
ls -ln /tmp/sid/logname | tr -s " " | cut -d" " -f1,3,4
sudo -u pcall /tmp/sid/logname
sudo -u pcall tsh -c /tmp/sid/logname

echo @@arc_ar
cd /tmp && printf 'aaa\n' > a.txt && printf 'bbbbbb\n' > b.txt && printf 'c\n' > a-very-long-member-name.txt
/bin/ar rcs libt.a a.txt b.txt a-very-long-member-name.txt; echo rc=$?
/bin/ar t libt.a; busybox ar t libt.a
/bin/ar p libt.a b.txt
mkdir -p x && cd x && /bin/ar x ../libt.a && cat a-very-long-member-name.txt && cd ..
/bin/ar d libt.a b.txt; /bin/ar t libt.a | wc -l
head -c 7 libt.a; echo
/bin/ar tv libt.a | tr -s ' ' | cut -d' ' -f1,3,8
cd /
echo @@arc_cpio
rm -rf /tmp/cp /tmp/cpx /tmp/t.cpio /tmp/bb.cpio /tmp/t.odc; mkdir -p /tmp/cp/sub /tmp/cpx; echo hi > /tmp/cp/f1; echo there > /tmp/cp/sub/f2
cd /tmp/cp; printf 'f1\nsub/f2\n' | /bin/cpio -o -H newc > /tmp/t.cpio 2>/dev/null; echo rc=$?; cd /
head -c 6 /tmp/t.cpio; echo
/bin/cpio -t < /tmp/t.cpio | sort; busybox cpio -t < /tmp/t.cpio 2>/dev/null | sort
cd /tmp/cpx; /bin/cpio -id < /tmp/t.cpio 2>&1; cat /tmp/cpx/sub/f2; cd /
cd /tmp/cp; printf 'f1\nsub/f2\n' | busybox cpio -o -H newc > /tmp/bb.cpio 2>/dev/null; cd /; /bin/cpio -t < /tmp/bb.cpio | sort | tr '\n' ' '; echo
cd /tmp/cp; printf 'f1\nsub/f2\n' | /bin/cpio -o -H odc > /tmp/t.odc 2>/dev/null; cd /; head -c 6 /tmp/t.odc; echo; /bin/cpio -t < /tmp/t.odc | wc -l
echo @@arc_xz_decode
/bin/unxz -c /etc/gnumade.xz | sha256sum | cut -c1-16
/bin/xzcat /etc/gnumade32.xz | sha256sum | cut -c1-16
/bin/xz -dc /etc/gnumadeblk.xz | sha256sum | cut -c1-16
sha256sum /etc/xzsrc.txt | cut -c1-16
/bin/xz -t /etc/gnumade.xz; echo rc=$?
/bin/xz -t /etc/gnubad.xz 2>&1; echo rc=$?
/bin/xz -l /etc/gnumadeblk.xz
echo @@arc_xz_encode
cp /etc/xzsrc.txt /tmp/xe.txt && /bin/xz -k /tmp/xe.txt; echo rc=$?
busybox unxz -c /tmp/xe.txt.xz > /tmp/xe1.out && cmp /tmp/xe1.out /etc/xzsrc.txt && echo "busybox reads native xz"
/bin/xz -dc /tmp/xe.txt.xz > /tmp/xe2.out && cmp /tmp/xe2.out /etc/xzsrc.txt && echo "round trip"
/bin/xz -c --check=crc32 /tmp/xe.txt | /bin/xz -d > /tmp/xe3.out && cmp /tmp/xe3.out /etc/xzsrc.txt && echo "crc32 pipe"
printf '' | /bin/xz | /bin/xz -d | wc -c
/bin/xz --version | head -1 | cut -d' ' -f1-3

echo @@ed_basic
cd /tmp && printf 'one\ntwo\nthree\n' > edt.txt
printf '2d\n$a\nfour\n.\n1s/one/ONE/\n,p\nw\nq\n' | /bin/ed -s edt.txt
cat edt.txt
echo @@ed_more
printf 'g/e/s/e/E/g\n,n\nu\n,p\nQ\n' | /bin/ed -s /tmp/edt.txt
printf '1,2m$\n,p\n2t0\n,p\n1,2j\n,p\n/four/\n=\nQ\n' | /bin/ed -s /tmp/edt.txt
printf 'H\nzz\nq\n' | /bin/ed -s /tmp/edt.txt | head -1
printf ',s/e/X/2\n,p\nQ\n' | /bin/ed -s /tmp/edt.txt
printf 'r !echo shell\n,p\nw /tmp/edout.txt\nQ\n' | /bin/ed -s /tmp/edt.txt; cat /tmp/edout.txt | wc -l
printf 'a\nline1\nline2\n.\nw /tmp/ednew.txt\nq\n' | /bin/ed -s; cat /tmp/ednew.txt
printf '1\nka\n3\n'"'"'ap\n,s/^/> /\n,p\nQ\n' | /bin/ed -s /tmp/edt.txt
cd /

echo @@cron_install
rm -f /tmp/cron_out
printf 'x * * * * true\n' | /bin/crontab - 2>&1; echo rc=$?
printf '# a comment\nSHELL=/bin/tsh\n* * * * * echo tick >> /tmp/cron_out\n@reboot echo boot >> /tmp/cron_out\n' | /bin/crontab -; echo rc=$?
/bin/crontab -l | grep -c .
i=0; while [ $i -lt 70 ] && ! grep -q tick /tmp/cron_out 2>/dev/null; do sleep 1; i=$((i+1)); done
grep -c tick /tmp/cron_out; grep -c boot /tmp/cron_out
/bin/crontab -r; echo rc=$?
/bin/crontab -l 2>&1; echo rc=$?

echo @@sys_modules
ls /lib/modules
/bin/rmmod hello > /dev/null 2>&1        # the kernel autoloads /lib/modules at boot
/bin/lsmod
/bin/insmod /lib/modules/hello.ko; echo rc=$?
/bin/lsmod | grep -c '^hello '
cut -d' ' -f1,5 /proc/modules
/bin/insmod /lib/modules/hello.ko 2>&1; echo rc=$?
/bin/rmmod hello; echo rc=$?
/bin/lsmod | grep -c hello
/bin/modprobe hello; echo rc=$?
/bin/modprobe -r hello; echo rc=$?
/bin/modprobe nonesuch 2>&1; echo rc=$?
/bin/rmmod hello 2>&1; echo rc=$?
sudo -u pcall /bin/insmod /lib/modules/hello.ko 2>&1; echo rc=$?

echo @@net_ip
/bin/ip addr show eth0 | sed 's/^ *//' | cut -d' ' -f1-3
/bin/ip -br addr | tr -s ' ' | cut -d' ' -f1,2
/bin/ip -br addr | grep -c '/'
/bin/ip link show eth0 | head -1 | cut -d' ' -f1-3
/bin/ip route | cut -d' ' -f1-4
/bin/ip -o link | wc -l
/bin/ip neigh > /dev/null; echo rc=$?
echo @@net_route_netstat
/bin/route -n | sed -n '2,3p' | tr -s ' ' | cut -d' ' -f1,4,8
/bin/netstat -rn | head -1
/bin/netstat -tln | head -2
/bin/netstat -i | head -3 | cut -d' ' -f1 | tr '\n' ' '; echo
/bin/netstat -an | grep -c '^tcp'
echo @@net_arp
GW=$(/bin/ip route | grep '^default' | cut -d' ' -f3); echo "gw=$GW"
ping -c1 -W2 $GW > /dev/null 2>&1
/bin/arp -n | sed 1d | tr -s ' ' | cut -d' ' -f1,2,3
/bin/arp -a | head -1 | cut -d' ' -f1-2
/bin/ip neigh | head -1 | tr -s ' ' | cut -d' ' -f1,2,3,5
/bin/arp -s 10.0.2.99 aa:bb:cc:dd:ee:ff; echo rc=$?
/bin/arp -n | grep -c 10.0.2.99
/bin/arp -d 10.0.2.99; echo rc=$?
/bin/arp -n | grep -c 10.0.2.99
/bin/arp -d 10.0.2.98 2>&1; echo rc=$?
echo @@net_arping
GW=$(/bin/ip route | grep '^default' | cut -d' ' -f3)
/bin/arping -c 2 -w 3 $GW > /tmp/ap.out; echo rc=$?; sed 's/  [0-9.]*ms/  T ms/; s/\[[0-9A-F:]*\]/[MAC]/' /tmp/ap.out
/bin/arping -c 1 -w 1 10.0.2.77 > /tmp/ap2.out; echo rc=$?; tail -1 /tmp/ap2.out
sudo -u pcall /bin/arping -c 1 $GW 2>&1; echo rc=$?
echo @@net_traceroute
GW=$(/bin/ip route | grep '^default' | cut -d' ' -f3)
/bin/traceroute -n -q 1 -w 2 -m 3 $GW > /tmp/tr.out 2>&1; echo rc=$?; sed 's/  [0-9.]* ms/  T ms/g' /tmp/tr.out
sudo -u pcall /bin/traceroute -n -m 1 $GW 2>&1; echo rc=$?
echo @@net_ip_set
GW=$(/bin/ip route | grep '^default' | cut -d' ' -f3)
/bin/ip route add 192.168.77.0/24 via $GW; echo rc=$?
/bin/ip route | grep -c 192.168.77
/bin/route del -net 192.168.77.0 netmask 255.255.255.0; echo rc=$?
/bin/ip route | grep -c 192.168.77
/bin/route add -host 192.168.78.1 gw $GW; echo rc=$?
/bin/route -n | grep -c 192.168.78.1
/bin/ip route del 192.168.78.1/32; echo rc=$?
/bin/ip link set eth0 up; echo rc=$?
sudo -u pcall /bin/ip route add 10.9.9.0/24 via $GW 2>&1; echo rc=$?
echo @@net_telnet
# QEMU's SLIRP does not loop the guest's own IP back, so telnet the loopback
# FTP server -- a fresh ftpd (-n 1: one connection then exit), the same target
# the ftp section uses at 127.0.0.1:21.
ftpd -n 1 -d /tmp/ftproot > /tmp/tftpd.log 2>&1 &
sleep 1
printf 'QUIT\r\n' | /bin/telnet 127.0.0.1 21 > /tmp/tn.out 2>&1; echo rc=$?; tr -d '\r' < /tmp/tn.out | sed 's/^220 .*/220 BANNER/; s/^221 .*/221 BYE/'
# a port with no listener: </dev/null so a stray connect cannot block on stdin
/bin/telnet 127.0.0.1 9 </dev/null > /tmp/tn2.out 2>&1; echo "refused_rc=$?"
head -1 /tmp/tn2.out

echo @@mkfs_disk
DEV=$(lsblk | grep '^ahci' | grep ' disk ' | tr -s ' ' | cut -d' ' -f1 | head -1); echo "dev=${DEV:-none}"
echo @@mkfs_ext2
/bin/mkfs.ext2 -q -L scratch $DEV; echo rc=$?
/bin/blkid /dev/$DEV | sed 's/UUID="[^"]*"/UUID="U"/'
e2fsck -n $DEV > /tmp/e2.out 2>&1; echo rc=$?; grep 'files (' /tmp/e2.out
dumpe2fs -h $DEV 2>/dev/null | grep -E 'volume name|Inode count|^Block count|magic number' | tr -s ' '
/bin/tune2fs -L renamed -c 25 -i 1w -e remount-ro $DEV > /tmp/t2.out; tail -4 /tmp/t2.out
/bin/tune2fs -l $DEV | grep -E 'volume name|Maximum mount|Check interval|Errors behavior' | tr -s ' '
/bin/fsck -t ext2 $DEV > /tmp/fk.out 2>&1; echo rc=$?; grep 'files (' /tmp/fk.out
echo @@mkfs_vfat
/bin/mkfs.vfat -n SCRATCH $DEV; echo rc=$?
/bin/blkid /dev/$DEV | sed 's/UUID="[^"]*"/UUID="U"/'
/bin/fsck.vfat $DEV > /tmp/fv.out; echo rc=$?; tail -1 /tmp/fv.out
/bin/fsck $DEV > /tmp/fv2.out 2>&1; grep 'files,' /tmp/fv2.out
/bin/mkfs.vfat -F 32 $DEV 2>/dev/null | head -1; /bin/blkid -s TYPE -o value $DEV; /bin/fsck.vfat $DEV > /tmp/fv3.out; tail -1 /tmp/fv3.out
echo @@mkfs_minix
/bin/mkfs.minix $DEV; echo rc=$?
/bin/blkid $DEV
/bin/fsck.minix $DEV > /tmp/fm.out; echo rc=$?; tail -2 /tmp/fm.out
/bin/mkfs.minix -2 $DEV | head -1; /bin/fsck.minix $DEV > /tmp/fm2.out; tail -2 /tmp/fm2.out
/bin/mkfs.minix -3 $DEV | head -1; /bin/fsck.minix $DEV > /tmp/fm3.out; tail -2 /tmp/fm3.out
echo @@mkfs_guard
/bin/mkfs.ext2 -q ide0:master 2>&1; echo rc=$?
sudo -u pcall /bin/mkfs.vfat $DEV > /tmp/mg.out 2>&1; echo rc=$?; grep 'mkfs.vfat:' /tmp/mg.out
/bin/mkfs.ext2 -q $DEV > /dev/null; e2fsck -n $DEV > /tmp/e2.out 2>&1; echo rc=$?; grep 'files (' /tmp/e2.out

echo @@diag_redir
# INFORMATIONAL (no check): `cmd > f 2>&1` followed by `tail -1 f` printed
# nothing in gate run 3 while sed/grep read the same file. Characterise it.
sh -c 'echo out; echo err >&2' > /tmp/d1 2>&1; echo "bytes=$(wc -c < /tmp/d1)"; cat /tmp/d1; echo "tail:[$(tail -1 /tmp/d1)]"; echo "bbtail:[$(busybox tail -1 /tmp/d1)]"
sh -c 'echo out' > /tmp/d2; echo "tail2:[$(tail -1 /tmp/d2)]"
sh -c 'echo out; echo err >&2' > /tmp/d3 2>&1; echo "tail3:[$(tail -n 1 /tmp/d3)]"; ls -l /tmp/d3 | tr -s ' ' | cut -d' ' -f5

echo @@teardown_users
userdel pcall > /dev/null 2>&1; echo del=$?
grep -c '^pcall:' /etc/passwd

echo @@dt_END
