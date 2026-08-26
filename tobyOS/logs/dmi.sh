#!/bin/bash
# dmi.sh -- the gate for SMBIOS discovery + /sys/firmware/dmi + native
# dmidecode.
#
#   bash logs/dmi.sh
#
# THREE boots, because one cannot separate the things that can go wrong:
#
#  [A] BIOS, QEMU's default SMBIOS. Proves the table is found and decoded.
#      On a BIOS boot the F-segment scan can succeed on its OWN, so this
#      run does NOT prove the Limine request id is right.
#
#  [B] BIOS, with SMBIOS strings QEMU is told to invent
#      (-smbios type=1,manufacturer=...). THE CONTROL: the values printed
#      must be the ones passed on the command line. A decoder returning
#      constants, or reading the wrong offsets, passes [A] and fails here.
#
#  [C] UEFI (OVMF). There is NO F-segment entry point under UEFI, so the
#      table can ONLY arrive through Limine's SMBIOS response. This is the
#      run that actually tests the request magic; without it a wrong id
#      would hide behind the fallback forever.
#
# Asserts VALUES throughout. "dmidecode exited 0" is worth nothing here --
# it exits 0 while printing a table full of zeroes.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs
QEMU="/c/Program Files/qemu/share/../../qemu/qemu-system-x86_64.exe"
[ -x "$QEMU" ] || QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
FLAGS="-DFAST_BOOT -DQUICK_BOOT -DPKGPROBE_BOOT"

# The [B] control strings. Deliberately not plausible defaults -- if any of
# these appears without QEMU being told to report it, something is wrong.
B_MANUF="TobySlice2Vendor"
B_PROD="DmiControlBoard"
B_SERIAL="SN-0BADCAFE-2"

taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1

echo "== build ($FLAGS)"
touch src/kernel.c
if ! make -j4 "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
             "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
             EXTRA_CFLAGS="$FLAGS" iso > logs/dmi.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/dmi.build.log | head -20
    exit 1
fi
python -c "
import sys
d=open('tobyos.bin','rb').read()
sys.exit(0 if b'/firmware/dmi/tables' in d and b'[smbios]' in d else 1)" \
    || { echo "FAIL: kernel lacks the DMI tree"; exit 1; }
tar -tf build/initrd.tar | grep -qx bin/dmidecode \
    || { echo "FAIL: /bin/dmidecode not in initrd"; exit 1; }
make "CC=TMP='C:\\t' TEMP='C:\\t' clang" build/ovmf-code.fd build/ovmf-vars.fd \
    >> logs/dmi.build.log 2>&1

run () {   # $1 = log, $2 = timeout seconds, rest = extra qemu args
    local log="$1"; local tmo="$2"; shift 2
    rm -f "$log"
    timeout "$tmo" "$QEMU" -cdrom tobyOS.iso -boot d -smp 4 -m 5120 \
        -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
        "$@" -serial file:"$log" -no-reboot -display none > /dev/null 2>&1
    taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1
}

echo "== [A] BIOS, default SMBIOS"
run logs/dmi_a.log 420
echo "== [B] BIOS, firmware told to report control strings"
run logs/dmi_b.log 420 \
    -smbios "type=1,manufacturer=$B_MANUF,product=$B_PROD,serial=$B_SERIAL"
# OVMF loads the (Chromium-bearing, ~900 MB) initrd MUCH slower than SeaBIOS.
# At 300s this run died inside "Loading module boot():/boot/initrd.tar" and
# looked like a UEFI failure when it was only a short clock.
echo "== [C] UEFI (OVMF) -- no F-segment, so Limine's response is the only route"
run logs/dmi_c.log 900 \
    -drive if=pflash,unit=0,format=raw,readonly=on,file=build/ovmf-code.fd \
    -drive if=pflash,unit=1,format=raw,file=build/ovmf-vars.fd

echo
echo "== what the guest printed (run A)"
tr -d '\r' < logs/dmi_a.log \
    | sed -n '/PKGPROBE. --- dmi tables present/,/PKGPROBE. ==== done/p' \
    | grep -vE '^\[(elf|proc|fork|execve|sys_exit)' | sed 's/^\[[0-9 ]*ms\] //'

echo
echo "== smbios discovery line, all three runs"
# grep -ao, not a whole-line grep: early boot output is not reliably
# newline-separated, so the LINE holding "[smbios]" can begin with an
# unrelated message ("vbe: Initialising...") and a whole-line grep then
# displays that instead. Extract the message itself.
# Capture into a variable too -- `grep ... | sed ... || echo "(none)"`
# takes SED's exit status, so the fallback never fires on no-match.
for t in a b c; do
    line=$(tr -d '\r' < logs/dmi_$t.log | grep -ao '\[smbios\] [^[]*' | head -1)
    printf '  [%s] %s\n' "$(echo $t | tr a-z A-Z)" "${line:-(no [smbios] line)}"
done

echo
echo "== checks (values, not exit codes)"
RC=0
ck () {  # $1 = log tag, $2 = description, $3 = regex
    if tr -d '\r' < "logs/dmi_$1.log" | grep -qaE "$3"; then
        echo "  ok   [$1] $2"
    else
        echo "  FAIL [$1] $2   (no match for: $3)"; RC=1
    fi
}
nck () { # must NOT match
    if tr -d '\r' < "logs/dmi_$1.log" | grep -qaE "$3"; then
        echo "  FAIL [$1] $2   (unexpectedly matched: $3)"; RC=1
    else
        echo "  ok   [$1] $2"
    fi
}

ck a "SMBIOS table found"            '\[smbios\] (limine-64|limine-32|f-scan): SMBIOS [0-9]+\.[0-9]+, table [0-9]+ bytes'
ck a "entry point + DMI published"   'smbios_entry_point'
ck a "dmidecode names the BIOS"      'BIOS Information'
ck a "BIOS vendor is a real string"  'Vendor: (SeaBIOS|EFI Development Kit|.*[A-Za-z]{3,})'
ck a "system + baseboard decoded"    'System Information'
ck a "sys_vendor id file"            'sys_vendor=[A-Za-z]'
ck a "memory device decoded"         'Memory Device'
ck a "dmidecode -t 0 exited 0"       'rc=0'
# This one was broken three ways before it was right, and each way is a
# lesson: `wc -l` PADS ("      69") so an anchored ^[0-9] never matched a
# correct answer; '{2,}' reached grep as TWO arguments (the second read as
# a filename); and the unanchored fallback '^ *[0-9][0-9]' matched 19
# unrelated numeric lines in the boot log, which would have passed no
# matter what dmidecode printed. The probe now emits a labelled value.
ck a "full run decoded 20+ lines"    'dmidecode-lines=[0-9][0-9]'

# [B] is the one that catches a decoder returning constants.
ck b "control MANUFACTURER round-trips" "Manufacturer: $B_MANUF"
ck b "control PRODUCT round-trips"      "Product Name: $B_PROD"
ck b "control SERIAL round-trips"       "Serial Number: $B_SERIAL"
ck b "control reaches /sys/class/dmi"   "sys_vendor=$B_MANUF"
ck b "dmidecode -s prints it bare"      "^$B_MANUF$"
nck a "control strings absent without -smbios" "$B_MANUF"

# [C] is the one that actually tests the Limine request id.
ck c "UEFI found the table VIA LIMINE" '\[smbios\] limine-(64|32): SMBIOS'
ck c "UEFI decoded a real vendor"      'BIOS Information'

for t in a b c; do
    if tr -d '\r' < logs/dmi_$t.log | grep -qaE 'KERNEL PANIC|PAGE FAULT|GENERAL PROTECTION'; then
        echo "  FAIL [$t] kernel faulted"; RC=1
    fi
done

echo
[ "$RC" = 0 ] && echo "RESULT: GREEN" || echo "RESULT: RED"
exit $RC
