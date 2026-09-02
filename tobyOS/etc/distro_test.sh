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
# /bin/arch is a busybox applet, so it runs under the LINUX personality --
# which is where the kernel's syscall stops live. Bounded, because every
# traced syscall is two context switches.
echo @@strace_run
timeout 40 strace /bin/arch 2>&1 | head -8
echo @@strace_exit
timeout 40 strace /bin/arch 2>&1 | grep "exited with" | head -1
echo @@strace_count
timeout 40 strace -c /bin/arch 2>&1 | head -16
echo @@strace_native
# A NATIVE tobyOS program, traced through the native dispatcher's stops --
# which did not exist until 2026-09-01; strace refused native binaries by
# name. The fixture exits 7, and that exit is a native syscall.
timeout 40 strace /etc/dbg_test.elf 2>&1 | tail -5

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

echo @@dt_END
