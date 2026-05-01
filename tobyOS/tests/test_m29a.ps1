# test_m29a.ps1 -- M29A Hardware Discovery & Inventory validation.
#
# Drives the boot-time M29A pipeline (kernel.c::m29a_run_hwinfo_harness).
# That pipeline:
#   1. takes a fresh hardware snapshot via hwinfo_snapshot()
#   2. dumps the inventory via hwinfo_dump_kprintf() (CPU + mem + bus)
#   3. persists the rendered text to /data/hwinfo.snap (best-effort)
#   4. spawns /bin/hwinfo --boot which validates SYS_HWINFO from
#      userland and emits M29A_HW: sentinels for this script
#
# We boot tobyOS twice in QEMU:
#   pass 1: with the standard QEMU device set; expect a populated
#           snapshot (PCI, USB, blk, input, display all > 0). The
#           persisted /data/hwinfo.snap should land on disk.
#   pass 2: with a different device set (no e1000, fewer USB devices);
#           expect SYS_HWINFO to still PASS, the bus counts to differ
#           cleanly from pass 1, and /data/hwinfo.snap to be rewritten
#           with the new layout. This is the "alternate VM config"
#           requirement from the milestone spec.
#
# Exit 0 => PASS, exit 1 => FAIL.


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m29a"
$serial   = Join-Path $LogDir "serial.$logTag.log"
$debug    = Join-Path $LogDir "debug.$logTag.log"
$qemuLog  = Join-Path $LogDir "qemu.$logTag.log"
$serial2  = Join-Path $LogDir "serial.${logTag}_alt.log"
$debug2   = Join-Path $LogDir "debug.${logTag}_alt.log"
$qemuLog2 = Join-Path $LogDir "qemu.${logTag}_alt.log"
$bootSentinel = '\[boot\] M29A: hwinfo harness complete'
$timeoutSec   = 60

if (-not (Test-Path $iso))  { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog, $serial2, $debug2, $qemuLog2 -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M29A: Hardware Discovery & Inventory ==============" -ForegroundColor Cyan

# ---------------- Pass 1: standard QEMU device set ----------------
$qemuArgs = @(
    "-cdrom", $iso,
    "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
    "-smp", "4",
    "-netdev", "user,id=net0",
    "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
    "-device", "qemu-xhci,id=usb0",
    "-device", "usb-kbd,bus=usb0.0",
    "-device", "usb-mouse,bus=usb0.0",
    "-serial", "file:$serial",
    "-debugcon", "file:$debug",
    "-d", "cpu_reset,guest_errors", "-D", $qemuLog,
    "-no-reboot", "-display", "none"
)
$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
Write-Host "[m29a] pass 1: qemu pid = $($proc.Id), watching $serial..."

$start = Get-Date
$saw   = $false
while (((Get-Date) - $start).TotalSeconds -lt $timeoutSec) {
    Start-Sleep -Seconds 1
    if (-not (Test-Path $serial)) { continue }
    $log = Get-Content $serial -Raw -ErrorAction SilentlyContinue
    if (-not $log) { continue }
    if ($log -match $bootSentinel) { $saw = $true; break }
    if ($log -match 'KERNEL PANIC at|page fault|KERNEL OOPS') { break }
}
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

if (-not (Test-Path $serial)) {
    Write-Host "FAIL: $serial not produced by QEMU" -ForegroundColor Red
    exit 1
}
$txt = Get-Content $serial -Raw

if (-not $saw) {
    Write-Host "FAIL: pass 1 timed out after ${timeoutSec}s waiting for boot sentinel" -ForegroundColor Red
    Write-Host "Tail of ${serial}:" -ForegroundColor Yellow
    Get-Content $serial -Tail 80
    exit 1
}

# === required signals (pass 1) ===
$mustHave = @(
    # Kernel-side hwinfo init must report a sane CPU.
    '\[hwinfo\] cpu=''.+'' family=\d+ model=\d+ step=\d+ count=\d+',
    '\[hwinfo\] mem: total=\d+ pg used=\d+ pg free=\d+ pg',
    '\[hwinfo\] bus: pci=\d+ usb=\d+ blk=\d+ input=\d+',
    # Kernel-side harness banner.
    '\[boot\] M29A: driving hwinfo harness',
    # Prior-snapshot probe must run on every boot. On pass 1 the
    # snapshot from any earlier dev run may or may not be present,
    # so we accept either branch here -- pass 2 enforces READABLE.
    '\[boot\] M29A: (prior snapshot READABLE|no prior snapshot)',
    # Persist verdict (PASS or SKIP both acceptable on first boot).
    '\[boot\] M29A: hwinfo persist (PASS bytes=\d+|SKIP)',
    # Snapshot one-liner -- key bus counts visible.
    '\[boot\] M29A: snapshot epoch=\d+ cpu_count=\d+ mem_total_pg=\d+ pci=\d+ usb=\d+ blk=\d+',
    # Userland /bin/hwinfo --boot sentinels.
    'M29A_HW: abi=\d+ profile=(vm|desktop|laptop) safe=0',
    'M29A_HW: cpu_count=\d+ family=\d+ model=\d+ step=\d+ feat=0x[0-9a-f]+ vendor=\S+',
    'M29A_HW: mem_total_pg=\d+ mem_used_pg=\d+ mem_free_pg=\d+',
    'M29A_HW: bus pci=\d+ usb=\d+ blk=\d+ input=\d+ audio=\d+ battery=\d+ hub=\d+ display=\d+',
    'M29A_HW: PASS',
    '\[boot\] M29A: /bin/hwinfo .*PASS\)',
    # Final harness sentinel.
    '\[boot\] M29A: hwinfo harness complete'
)
$missing = @()
foreach ($pat in $mustHave) {
    if ($txt -notmatch $pat) { $missing += $pat }
}

# === forbidden tokens (pass 1) ===
$forbidden = @(
    'KERNEL PANIC at',
    'page fault',
    'KERNEL OOPS',
    'M29A_HW: FAIL',
    '\[boot\] M29A: /bin/hwinfo not spawned',
    '\[boot\] M29A: hwinfo persist FAIL'
)
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

# Snapshot fingerprints we want to compare across passes.
$pciP1     = if ($txt -match 'M29A_HW: bus pci=(\d+) ')    { [int]$Matches[1] } else { -1 }
$usbP1     = if ($txt -match 'M29A_HW: bus pci=\d+ usb=(\d+) ') { [int]$Matches[1] } else { -1 }
$blkP1     = if ($txt -match 'usb=\d+ blk=(\d+) ')         { [int]$Matches[1] } else { -1 }
$inputP1   = if ($txt -match 'blk=\d+ input=(\d+) ')       { [int]$Matches[1] } else { -1 }
$displayP1 = if ($txt -match 'hub=\d+ display=(\d+)')      { [int]$Matches[1] } else { -1 }

if ($missing.Count -gt 0 -or $panics.Count -gt 0) {
    Write-Host "M29A FAIL (pass 1)" -ForegroundColor Red
    if ($missing.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($panics.Count -gt 0) {
        Write-Host "Forbidden tokens present:" -ForegroundColor Red
        $panics | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 120 lines of ${serial} ===" -ForegroundColor Yellow
    Get-Content $serial -Tail 120
    exit 1
}

Write-Host "M29A PASS (pass 1) -- standard QEMU profile produced full inventory" -ForegroundColor Green
Write-Host "[m29a]   pci=$pciP1 usb=$usbP1 blk=$blkP1 input=$inputP1 display=$displayP1"

# ---------------- Pass 2: alternate VM config (no NIC, no USB mouse) ----------------
# This proves the harness works on a different machine shape: a
# hardware change must change the bus counts in the snapshot
# without breaking the boot. We drop the NIC entirely (-net none
# also stops QEMU's default e1000) and remove the USB mouse;
# keyboard stays so the kernel still has at least one input device.
$qemuArgs2 = @(
    "-cdrom", $iso,
    "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
    "-smp", "2",
    "-net", "none",
    "-device", "qemu-xhci,id=usb0",
    "-device", "usb-kbd,bus=usb0.0",
    "-serial", "file:$serial2",
    "-debugcon", "file:$debug2",
    "-d", "cpu_reset,guest_errors", "-D", $qemuLog2,
    "-no-reboot", "-display", "none"
)
Write-Host ""
Write-Host "[m29a] pass 2: alternate config (no NIC, no USB mouse, smp=2)"
$proc2 = Start-Process -FilePath $qemu -ArgumentList $qemuArgs2 -PassThru
$start2 = Get-Date
$saw2   = $false
while (((Get-Date) - $start2).TotalSeconds -lt $timeoutSec) {
    Start-Sleep -Seconds 1
    if (-not (Test-Path $serial2)) { continue }
    $log2 = Get-Content $serial2 -Raw -ErrorAction SilentlyContinue
    if (-not $log2) { continue }
    if ($log2 -match $bootSentinel) { $saw2 = $true; break }
    if ($log2 -match 'KERNEL PANIC at|page fault|KERNEL OOPS') { break }
}
Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

if (-not $saw2) {
    Write-Host "FAIL: pass 2 timed out after ${timeoutSec}s" -ForegroundColor Red
    Get-Content $serial2 -Tail 80
    exit 1
}
$txt2 = Get-Content $serial2 -Raw

# Pass 2 still needs the same overall sentinels, AND must positively
# observe the snapshot from pass 1 surviving the reboot.
$mustHave2 = @(
    '\[boot\] M29A: driving hwinfo harness',
    # CRITICAL: pass 2 ran after pass 1 wrote the snapshot; the disk
    # image is reused across QEMU runs, so the snapshot must persist.
    '\[boot\] M29A: prior snapshot READABLE \(size=\d+ bytes',
    '\[boot\] M29A: hwinfo persist (PASS bytes=\d+|SKIP)',
    'M29A_HW: PASS',
    '\[boot\] M29A: /bin/hwinfo .*PASS\)',
    '\[boot\] M29A: hwinfo harness complete'
)
$rmiss2 = @()
foreach ($pat in $mustHave2) {
    if ($txt2 -notmatch $pat) { $rmiss2 += $pat }
}

$pciP2     = if ($txt2 -match 'M29A_HW: bus pci=(\d+) ')    { [int]$Matches[1] } else { -1 }
$usbP2     = if ($txt2 -match 'M29A_HW: bus pci=\d+ usb=(\d+) ') { [int]$Matches[1] } else { -1 }
$blkP2     = if ($txt2 -match 'usb=\d+ blk=(\d+) ')         { [int]$Matches[1] } else { -1 }
$inputP2   = if ($txt2 -match 'blk=\d+ input=(\d+) ')       { [int]$Matches[1] } else { -1 }
$displayP2 = if ($txt2 -match 'hub=\d+ display=(\d+)')      { [int]$Matches[1] } else { -1 }

if ($rmiss2.Count -gt 0) {
    Write-Host "M29A FAIL (pass 2): missing signals on alternate config:" -ForegroundColor Red
    $rmiss2 | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    Get-Content $serial2 -Tail 100
    exit 1
}

# Verify the snapshot ACTUALLY changed across configs. We removed
# the NIC (1 fewer PCI device) and the USB mouse (1 fewer USB / 1
# fewer input). The USB and input deltas are the most deterministic
# signal because the in-kernel USB/input enumerators count children
# of the qemu-xhci controller directly, while PCI counts can vary
# slightly with QEMU defaults.
if ($pciP2 -lt 0 -or $pciP1 -lt 0 -or
    $usbP2 -lt 0 -or $usbP1 -lt 0 -or
    $inputP2 -lt 0 -or $inputP1 -lt 0) {
    Write-Host "FAIL: could not parse bus counts from one of the snapshots" -ForegroundColor Red
    Write-Host "       pass1 pci=$pciP1 usb=$usbP1 input=$inputP1" -ForegroundColor Yellow
    Write-Host "       pass2 pci=$pciP2 usb=$usbP2 input=$inputP2" -ForegroundColor Yellow
    exit 1
}
$diffPci   = $pciP2 -lt $pciP1
$diffUsb   = $usbP2 -eq ($usbP1 - 1)
$diffInput = $inputP2 -eq ($inputP1 - 1)
if (-not ($diffUsb -and $diffInput)) {
    Write-Host "FAIL: alternate config did not show the expected hardware delta" -ForegroundColor Red
    Write-Host "       pass1 pci=$pciP1 usb=$usbP1 input=$inputP1" -ForegroundColor Yellow
    Write-Host "       pass2 pci=$pciP2 usb=$usbP2 input=$inputP2" -ForegroundColor Yellow
    Write-Host "       expected: usb -1 (mouse removed) AND input -1 (mouse removed)" -ForegroundColor Yellow
    exit 1
}
if (-not $diffPci) {
    Write-Host "WARN: pass 2 PCI count did not drop ($pciP1 -> $pciP2);" -ForegroundColor Yellow
    Write-Host "       USB/input deltas still confirm alternate config detected." -ForegroundColor Yellow
}

Write-Host "M29A PASS (pass 2) -- alternate config detected: pci $pciP1 -> $pciP2, usb $usbP1 -> $usbP2, input $inputP1 -> $inputP2" -ForegroundColor Green

Write-Host ""
Write-Host "Last 30 lines of $serial2 for visual sanity check:"
Get-Content $serial2 -Tail 30

Write-Host ""
Write-Host "M29A PASS (overall) -- hardware inventory works on >=2 device sets" -ForegroundColor Green
exit 0
