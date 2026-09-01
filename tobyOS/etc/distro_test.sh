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

echo @@dt_END
