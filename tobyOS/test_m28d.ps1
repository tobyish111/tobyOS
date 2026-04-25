# test_m28d.ps1 -- M28D Safe Mode Boot validation.
#
# Strategy: two boots against the same disk image.
#
#   Boot 1 ("safe boot"):
#     - Rebuild the initrd with SAFEMODE_FLAG=1, baking
#       /etc/safemode_now into the read-only initrd.
#     - Boot QEMU. safemode_init() latches g_active=true. Subsequent
#       init code:
#         * skips net_init, gfx_layer_init, gui_init, term_init,
#           m14_init (settings/services/login/desktop), pkg_init,
#           devtest harnesses, M26A/M27A userland tools.
#         * KEEPS slog, watchdog, panic, PIT, kbd, VFS, /data mount.
#       Then spawns /bin/safesh --boot which validates that:
#         * SYS_SLOG_READ + SYS_SLOG_WRITE work,
#         * SYS_WDOG_STATUS works and the watchdog is ticking
#           (kernel + sched + syscall heartbeats > 0).
#       Prints M28D_SAFESH: PASS.
#     - Verify the serial log proves both that we DID skip the
#       optional subsystems AND that the essentials are alive.
#
#   Boot 2 ("normal regression"):
#     - Rebuild the initrd WITHOUT SAFEMODE_FLAG.
#     - Boot QEMU and verify the normal boot path still completes
#       (no regression introduced by the safe-mode gates). We just
#       wait for the M28A logging-harness sentinel and ensure no
#       safe-mode skip lines appear.
#
# Exit 0 => PASS, exit 1 => FAIL.

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m28d"
$timeoutSec = 90

if (-not (Test-Path $qemu)) {
    Write-Host "qemu not found at $qemu" -ForegroundColor Red; exit 1
}
Set-Location -Path (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Build-Iso {
    param([string]$flag)
    $ttag = if ($flag) { "with SAFEMODE_FLAG=$flag" } else { "stock" }
    Write-Host "[m28d] make ($ttag) ..." -ForegroundColor Cyan
    if ($flag) {
        & bash -lc "PATH=/c/msys64/ucrt64/bin:`$PATH; cd /c/CustomOS/tobyOS && rm -f build/initrd.tar build/base.iso build/install.img tobyOS.iso && SAFEMODE_FLAG=$flag make 2>&1 | tail -5"
    } else {
        & bash -lc "PATH=/c/msys64/ucrt64/bin:`$PATH; cd /c/CustomOS/tobyOS && rm -f build/initrd.tar build/base.iso build/install.img tobyOS.iso && make 2>&1 | tail -5"
    }
    if (-not (Test-Path $iso)) {
        Write-Host "FAIL: build did not produce $iso" -ForegroundColor Red
        exit 1
    }
}

function Ensure-Disk {
    if (-not (Test-Path $disk)) {
        Write-Host "[m28d] disk.img missing -- running 'make disk.img' to create..." -ForegroundColor Yellow
        & bash -lc "PATH=/c/msys64/ucrt64/bin:`$PATH; cd /c/CustomOS/tobyOS && make disk.img 2>&1 | tail -5"
    }
    if (-not (Test-Path $disk)) {
        Write-Host "FAIL: cannot create $disk" -ForegroundColor Red
        exit 1
    }
}

function Run-Qemu {
    param(
        [string]$serialFile,
        [string]$debugFile,
        [string]$qemuFile,
        [string]$endPattern,
        [int]$timeoutSec
    )
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    Remove-Item -Force $serialFile, $debugFile, $qemuFile -ErrorAction SilentlyContinue

    $qemuArgs = @(
        "-cdrom", $iso,
        "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
        "-smp", "4",
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
        "-device", "qemu-xhci,id=usb0",
        "-device", "usb-kbd,bus=usb0.0",
        "-device", "usb-mouse,bus=usb0.0",
        "-serial", "file:$serialFile",
        "-debugcon", "file:$debugFile",
        "-d", "cpu_reset,guest_errors", "-D", $qemuFile,
        "-no-reboot", "-display", "none"
    )
    $proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
    Write-Host "[m28d] qemu pid=$($proc.Id) waiting for: $endPattern (timeout=${timeoutSec}s)"
    $start = Get-Date
    $saw   = $false
    while (((Get-Date) - $start).TotalSeconds -lt $timeoutSec) {
        Start-Sleep -Seconds 1
        if (-not (Test-Path $serialFile)) { continue }
        $log = Get-Content $serialFile -Raw -ErrorAction SilentlyContinue
        if (-not $log) { continue }
        if ($log -match $endPattern) { $saw = $true; break }
    }
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
    return $saw
}

Write-Host ""
Write-Host "============== M28D: Safe Mode Boot ==============" -ForegroundColor Cyan

# --- Boot 1: safe mode ---
Build-Iso "1"
Ensure-Disk

$serialA = "serial.${logTag}_safe.log"
$debugA  = "debug.${logTag}_safe.log"
$qemuA   = "qemu.${logTag}_safe.log"
$saw1 = Run-Qemu -serialFile $serialA -debugFile $debugA -qemuFile $qemuA `
                 -endPattern 'M28D_SAFESH: PASS|M28D_SAFESH: FAIL' `
                 -timeoutSec $timeoutSec

if (-not (Test-Path $serialA)) {
    Write-Host "FAIL: safe-mode boot did not produce $serialA" -ForegroundColor Red
    exit 1
}
$txt1 = Get-Content $serialA -Raw
if (-not $saw1) {
    Write-Host "FAIL: safe-mode boot did not surface safesh verdict within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serialA}:" -ForegroundColor Yellow
    Get-Content $serialA -Tail 80
    exit 1
}

# Required signals on safe boot.
$safeRequired = @(
    # Safe-mode init detected the flag.
    '\[safe\] /etc/safemode_now present -- SAFE MODE ACTIVE',
    # Each non-essential subsystem confirmed skipped.
    '\[safe\] skipping net_init',
    '\[safe\] skipping gfx/mouse/gui/term/m14_init',
    '\[safe\] skipping pkg_init \+ selftests \+ devtest harnesses',
    # Essentials kept: M28A logging harness still runs in safe mode.
    '\[boot\] M28A: logging harness complete',
    # Safe-mode shell harness fires.
    '\[boot\] M28D: SAFE MODE -- spawning /bin/safesh',
    # Userland verdicts.
    'M28D_SAFESH: starting \(safe-mode shell harness\)',
    'M28D_SAFESH: slog_read=ok records=\d+',
    'M28D_SAFESH: wdog_alive=1',
    'M28D_SAFESH: slog_write=ok',
    'M28D_SAFESH: PASS',
    '\[boot\] M28D: /bin/safesh \(pid=\d+\) exit=0 \(PASS\)',
    '\[boot\] M28D: safe-mode harness complete'
)

# Forbidden tokens on safe boot: anything that means a non-essential
# subsystem ran when it should have been skipped.
$safeForbidden = @(
    'KERNEL PANIC at',
    'KERNEL OOPS',
    # GUI launching desktop = means we did NOT skip the compositor.
    '\[gui\] launched /bin/login',
    # Network init banner.
    '\[net\] up: link',
    # M27A display tools should NOT have run.
    'M27A_DISPLAYINFO: PASS',
    'M28D_SAFESH: FAIL'
)

$missing1 = @()
foreach ($pat in $safeRequired) {
    if ($txt1 -notmatch $pat) { $missing1 += $pat }
}
$bad1 = @()
foreach ($pat in $safeForbidden) {
    if ($txt1 -match $pat) { $bad1 += $pat }
}

if ($missing1.Count -gt 0 -or $bad1.Count -gt 0) {
    Write-Host "M28D FAIL (safe boot)" -ForegroundColor Red
    if ($missing1.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing1 | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($bad1.Count -gt 0) {
        Write-Host "Forbidden tokens (subsystem leaked through safe-mode gate):" -ForegroundColor Red
        $bad1 | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 200 lines of $serialA ===" -ForegroundColor Yellow
    Get-Content $serialA -Tail 200
    exit 1
}
Write-Host "M28D PASS (safe boot) -- non-essentials skipped, essentials alive, safesh verified." -ForegroundColor Green

# --- Boot 2: normal regression ---
Build-Iso ""
$serialB = "serial.${logTag}_normal.log"
$debugB  = "debug.${logTag}_normal.log"
$qemuB   = "qemu.${logTag}_normal.log"
$saw2 = Run-Qemu -serialFile $serialB -debugFile $debugB -qemuFile $qemuB `
                 -endPattern '\[boot\] M28A: logging harness complete' `
                 -timeoutSec $timeoutSec

if (-not (Test-Path $serialB)) {
    Write-Host "FAIL: normal boot did not produce $serialB" -ForegroundColor Red
    exit 1
}
$txt2 = Get-Content $serialB -Raw
if (-not $saw2) {
    Write-Host "FAIL: normal boot did not reach M28A complete within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serialB}:" -ForegroundColor Yellow
    Get-Content $serialB -Tail 80
    exit 1
}

# Normal-boot regression checks.
$normRequired = @(
    '\[safe\] no /etc/safemode_now -- normal boot',
    '\[boot\] M28A: logging harness complete'
)
$normForbidden = @(
    '\[safe\] /etc/safemode_now present -- SAFE MODE ACTIVE',
    '\[safe\] skipping ',
    '\[boot\] M28D: SAFE MODE -- spawning /bin/safesh',
    'KERNEL PANIC at',
    'KERNEL OOPS'
)
$missing2 = @()
foreach ($pat in $normRequired) {
    if ($txt2 -notmatch $pat) { $missing2 += $pat }
}
$bad2 = @()
foreach ($pat in $normForbidden) {
    if ($txt2 -match $pat) { $bad2 += $pat }
}

if ($missing2.Count -gt 0 -or $bad2.Count -gt 0) {
    Write-Host "M28D FAIL (normal regression)" -ForegroundColor Red
    if ($missing2.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing2 | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($bad2.Count -gt 0) {
        Write-Host "Forbidden tokens:" -ForegroundColor Red
        $bad2 | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 200 lines of $serialB ===" -ForegroundColor Yellow
    Get-Content $serialB -Tail 200
    exit 1
}

Write-Host "M28D PASS (normal regression) -- normal boot still healthy." -ForegroundColor Green
Write-Host ""
Write-Host "=== last 30 lines of $serialA ===" -ForegroundColor DarkGray
Get-Content $serialA -Tail 30
Write-Host ""
Write-Host "M28D PASS (overall)" -ForegroundColor Green
exit 0
