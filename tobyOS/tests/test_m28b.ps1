# test_m28b.ps1 -- M28B Kernel Panic + Crash Dumps validation.
#
# Strategy: two boots against the same disk image.
#
#   Boot 1 ("crashtest" boot):
#     - Rebuild the initrd with CRASHTEST_FLAG=1, which bakes
#       /etc/crashtest_now into the read-only initrd.
#     - Boot QEMU. Right after the M28A logging harness finishes,
#       kernel.c sees /etc/crashtest_now exists, triggers
#       kpanic_self_test("kpanic"), and the M28B panic path takes
#       over: paint panic screen, print regs+process+stack+slog tail,
#       try_write_crash_dump() into /data/crash/last.dump, then
#       hlt_forever().
#     - Verify the serial log contains the panic banner, register
#       dump, slog tail, and "Crash dump saved" line.
#
#   Boot 2 ("inspect" boot):
#     - Rebuild the initrd WITHOUT CRASHTEST_FLAG so we boot normally.
#     - Run /bin/crashinfo --boot. It opens /data/crash/last.dump,
#       validates the abi_crash_header (magic, version, body_bytes),
#       prints the M28B_CRASHINFO sentinels.
#     - Verify the dump survived the panic + reboot.
#
# This proves: panic handler runs end-to-end, crash dump survives a
# crash, and the on-disk format is compatible with userland tooling.
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
$logTag   = "m28b"
$serial   = Join-Path $LogDir "serial.$logTag.log"
$debug    = Join-Path $LogDir "debug.$logTag.log"
$qemuLog  = Join-Path $LogDir "qemu.$logTag.log"
$timeoutSec = 60

# Sentinels we use to know when to terminate QEMU on the crashtest boot.
# kpanic_at() prints a closing "==" banner with "System halted." after
# all the diagnostics. Once that's in the serial log we know the dump
# attempt has already happened (it runs earlier in the function).
$panicEndSentinel = 'System halted\.'

if (-not (Test-Path $qemu))  { Write-Host "qemu not found at $qemu" -ForegroundColor Red; exit 1 }

# The script lives in c:\CustomOS\tobyOS\ -- run from there.
Set-Location -Path (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Build-Iso {
    param([string]$flag)
    $ttag = if ($flag) { "with CRASHTEST_FLAG=$flag" } else { "stock" }
    Write-Host "[m28b] make ($ttag) ..." -ForegroundColor Cyan
    if ($flag) {
        # Force initrd rebuild so the flag file flips on/off.
        & bash -lc "PATH=/c/msys64/ucrt64/bin:`$PATH; cd /c/CustomOS/tobyOS && rm -f build/initrd.tar build/base.iso build/install.img tobyOS.iso && CRASHTEST_FLAG=$flag make 2>&1 | tail -5"
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
        Write-Host "[m28b] disk.img missing -- running 'make disk.img' to create..." -ForegroundColor Yellow
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
    Write-Host "[m28b] qemu pid=$($proc.Id) waiting for: $endPattern (timeout=${timeoutSec}s)"
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
Write-Host "============== M28B: Kernel Panic + Crash Dumps ==============" -ForegroundColor Cyan

# --- Boot 1: trigger controlled panic ---
Build-Iso "1"
Ensure-Disk

$serialA = Join-Path $LogDir "serial.${logTag}_crash.log"
$debugA  = Join-Path $LogDir "debug.${logTag}_crash.log"
$qemuA   = Join-Path $LogDir "qemu.${logTag}_crash.log"
$saw1 = Run-Qemu -serialFile $serialA -debugFile $debugA -qemuFile $qemuA `
                 -endPattern $panicEndSentinel -timeoutSec $timeoutSec

if (-not (Test-Path $serialA)) {
    Write-Host "FAIL: crashtest boot did not produce $serialA" -ForegroundColor Red
    exit 1
}
$txt1 = Get-Content $serialA -Raw
if (-not $saw1) {
    Write-Host "FAIL: crashtest boot did not reach panic handler within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serialA}:" -ForegroundColor Yellow
    Get-Content $serialA -Tail 80
    exit 1
}

$crashRequired = @(
    # M28A still runs and completes before we trip the panic.
    '\[boot\] M28A: logging harness complete',
    # The boot harness sees the flag file.
    '\[boot\] M28B: /etc/crashtest_now present -- triggering controlled panic for test',
    # kpanic_self_test prints the trigger line.
    '\[crashtest\] kpanic triggering direct panic\.\.\.',
    # The panic banner.
    'KERNEL PANIC at .*src/panic\.c:\d+',
    'reason: crashtest: direct kpanic_self_test\(.kpanic.\)',
    # Process info.
    'Current process:',
    # Register dump (look for several headers, not all reg names).
    'Registers:',
    'rax=[0-9a-f]{16} rbx=[0-9a-f]{16}',
    'rip=[0-9a-f]{16} rflags=[0-9a-f]{16}',
    'cr0=[0-9a-f]{16} cr2=[0-9a-f]{16}',
    # Stack trace banner (frames may be empty if FPs are elided -- still ok).
    'Stack trace \(frame-walk',
    # SLOG tail in the panic body.
    'Recent slog records:',
    # Crash dump saved -- the only path that proves disk write succeeded.
    'Crash dump saved to /data/crash/last\.dump \(\d+ bytes\)',
    # Halt banner.
    'System halted\.'
)
$missing1 = @()
foreach ($pat in $crashRequired) {
    if ($txt1 -notmatch $pat) { $missing1 += $pat }
}

if ($missing1.Count -gt 0) {
    Write-Host "M28B FAIL (crashtest boot)" -ForegroundColor Red
    Write-Host "Missing signals:" -ForegroundColor Red
    $missing1 | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "=== last 200 lines of $serialA ===" -ForegroundColor Yellow
    Get-Content $serialA -Tail 200
    exit 1
}
Write-Host "M28B PASS (crashtest boot) -- panic handler produced banner, regs, slog tail, and crash dump." -ForegroundColor Green

# --- Boot 2: rebuild WITHOUT the flag, run /bin/crashinfo --boot ---
Build-Iso ""

$serialB = Join-Path $LogDir "serial.${logTag}_inspect.log"
$debugB  = Join-Path $LogDir "debug.${logTag}_inspect.log"
$qemuB   = Join-Path $LogDir "qemu.${logTag}_inspect.log"

# We need crashinfo to be launched on this boot. We'll piggyback on the
# M28A boot harness to also exec /bin/crashinfo --boot when we are NOT
# in crashtest mode. (Implemented in kernel.c right after M28A.)
$inspectSentinel = 'M28B_CRASHINFO: PASS|M28B_CRASHINFO: FAIL'
$saw2 = Run-Qemu -serialFile $serialB -debugFile $debugB -qemuFile $qemuB `
                 -endPattern $inspectSentinel -timeoutSec $timeoutSec

if (-not (Test-Path $serialB)) {
    Write-Host "FAIL: inspect boot did not produce $serialB" -ForegroundColor Red
    exit 1
}
$txt2 = Get-Content $serialB -Raw
if (-not $saw2) {
    Write-Host "FAIL: inspect boot did not surface crashinfo verdict within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serialB}:" -ForegroundColor Yellow
    Get-Content $serialB -Tail 80
    exit 1
}

$inspectRequired = @(
    # M28A logging harness must still pass on a clean boot (proves we
    # didn't break the normal boot path with M28B changes).
    '\[boot\] M28A: logging harness complete',
    # NO crashtest trigger this time -- we should NOT see the panic.
    # (We just guard against an accidental panic by checking forbidden tokens below.)
    # crashinfo --boot output: header decode + reason + body preview + verdict.
    'M28B_CRASHINFO: header\.magic=0x48535243 version=1 body_bytes=\d+',
    'M28B_CRASHINFO: reason="crashtest: direct kpanic_self_test',
    'M28B_CRASHINFO: body_preview_begin',
    'M28B_CRASHINFO: body_preview_end',
    'M28B_CRASHINFO: heuristics regs=1 slog=1',
    'M28B_CRASHINFO: PASS'
)
$missing2 = @()
foreach ($pat in $inspectRequired) {
    if ($txt2 -notmatch $pat) { $missing2 += $pat }
}

# On the inspect boot, no panic should occur.
$forbidden = @(
    'KERNEL PANIC at',
    'page fault',
    'KERNEL OOPS',
    'M28B_CRASHINFO: FAIL'
)
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt2 -match $pat) { $panics += $pat }
}

if ($missing2.Count -gt 0 -or $panics.Count -gt 0) {
    Write-Host "M28B FAIL (inspect boot)" -ForegroundColor Red
    if ($missing2.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing2 | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($panics.Count -gt 0) {
        Write-Host "Forbidden tokens (clean boot wasn't clean):" -ForegroundColor Red
        $panics | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 200 lines of $serialB ===" -ForegroundColor Yellow
    Get-Content $serialB -Tail 200
    exit 1
}

Write-Host "M28B PASS (inspect boot) -- crash dump survived reboot and decoded successfully." -ForegroundColor Green
Write-Host ""
Write-Host "=== last 30 lines of $serialB ===" -ForegroundColor DarkGray
Get-Content $serialB -Tail 30
Write-Host ""
Write-Host "M28B PASS (overall)" -ForegroundColor Green
exit 0
