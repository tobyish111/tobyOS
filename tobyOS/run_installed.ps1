# run_installed.ps1 -- boot the disk image that the milestone-20
# installer flashed, WITHOUT a CD-ROM. Limine's hybrid MBR (stamped
# in sector 0 by the installer) chainloads the Limine stage-2 out of
# the ISO9660 data inside the first 4 MiB of the disk; the kernel
# then mounts /data from the tobyfs region at LBA 8192.
#
# Expected flow:
#   1.  make                     # build tobyOS.iso + disk.img
#   2.  make run                 # boot live ISO (disk.img is target)
#   3.  at the shell prompt:
#         install --yes          # flash install.img onto disk.img
#       ...wait for "install: SUCCESS", then close QEMU.
#   4.  .\run_installed.ps1      # boots the installed disk directly
#
# Output:
#   serial.log    - kernel + shell output (COM1)
#   debug.log     - kprintf debugcon mirror
#   qemu.log      - QEMU -d int traces (filtered)

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

$disk = 'disk.img'
if ($args.Count -ge 1) { $disk = $args[0] }

if (-not (Test-Path $disk)) {
    Write-Host "error: $disk not found -- run the installer first (see comment at top)"
    exit 1
}

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -ErrorAction SilentlyContinue serial.log, debug.log, qemu.log, net.pcap

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$qemuArgs = @(
    "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
    "-boot", "c",
    "-smp", "4",
    "-netdev", "user,id=net0,hostfwd=udp::5555-:5555",
    "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
    "-object", "filter-dump,id=fd0,netdev=net0,file=net.pcap",
    "-serial", "file:serial.log",
    "-debugcon", "file:debug.log",
    "-d", "int,cpu_reset,guest_errors", "-D", "qemu.log",
    "-no-reboot", "-no-shutdown"
)

Write-Host "Booting installed disk: $disk (no CD-ROM)"
& $qemu @qemuArgs
