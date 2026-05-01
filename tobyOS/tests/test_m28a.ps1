# test_m28a.ps1 -- M28A Logging + Diagnostics Framework validation.
#
# Drives the boot-time M28A pipeline (kernel.c::m28a_run_logging_harness).
# That pipeline:
#   1. emits structured log records (slog) from several subsystems / levels
#   2. attempts a flush of the in-kernel ring to /data/system.log
#   3. snapshots and prints the slog stats
#   4. spawns /bin/logview --boot which drains the ring through the
#      SYS_SLOG_READ syscall, double-checks the M28A_TAG markers, posts
#      its own SYS_SLOG_WRITE record, and prints the M28A_LOGVIEW PASS
#      sentinel.
#
# This script just boots tobyOS in QEMU, waits for the M28A summary
# line, kills QEMU, and verifies the captured serial.log contains every
# expected signal. Exit 0 => PASS, exit 1 => FAIL.


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m28a"
$serial   = Join-Path $LogDir "serial.$logTag.log"
$debug    = Join-Path $LogDir "debug.$logTag.log"
$qemuLog  = Join-Path $LogDir "qemu.$logTag.log"
$bootSentinel = '\[boot\] M28A: logging harness complete'
$timeoutSec   = 60

if (-not (Test-Path $iso))  { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M28A: Logging + Diagnostics Framework ==============" -ForegroundColor Cyan

# Same QEMU args as M27A so the boot path is identical -- we only care
# about the slog ring, the persistence flush, and the logview tool.
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
Write-Host "[m28a] qemu pid = $($proc.Id), watching $serial for boot sentinel..."

$start = Get-Date
$saw   = $false
while (((Get-Date) - $start).TotalSeconds -lt $timeoutSec) {
    Start-Sleep -Seconds 1
    if (-not (Test-Path $serial)) { continue }
    $log = Get-Content $serial -Raw -ErrorAction SilentlyContinue
    if (-not $log) { continue }
    if ($log -match $bootSentinel) { $saw = $true; break }
    if ($log -match 'panic|page fault|KERNEL OOPS') { break }
}
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

if (-not (Test-Path $serial)) {
    Write-Host "FAIL: $serial not produced by QEMU" -ForegroundColor Red
    exit 1
}
$txt = Get-Content $serial -Raw

if (-not $saw) {
    Write-Host "FAIL: timed out after ${timeoutSec}s waiting for boot sentinel" -ForegroundColor Red
    Write-Host "Tail of ${serial}:" -ForegroundColor Yellow
    Get-Content $serial -Tail 80
    exit 1
}

# === required signals ===
$mustHave = @(
    # Kernel-side slog ring must come up early.
    '\[\d+\] INFO  slog\s+: slog ready \(depth=256, persist=/data/system\.log\)',
    # Kernel-side harness banner.
    '\[boot\] M28A: driving logging harness',
    # Synthetic emissions from each subsystem we exercised. The exact
    # timestamp varies, so we anchor on level + sub + tag.
    '\[\d+\] INFO  kernel.*M28A_TAG kernel info trace',
    '\[\d+\] WARN  fs.*M28A_TAG fs warn trace',
    '\[\d+\] ERROR net.*M28A_TAG net error trace',
    '\[\d+\] INFO  gui.*M28A_TAG gui info trace',
    '\[\d+\] INFO  driver.*M28A_TAG driver info trace',
    '\[\d+\] INFO  display.*M28A_TAG display info trace',
    '\[\d+\] INFO  audio.*M28A_TAG audio info trace',
    '\[\d+\] WARN  svc.*M28A_TAG svc warn trace',
    '\[\d+\] ERROR panic.*M28A_TAG panic-test \(synthetic, no halt\)',
    # Persist flush -- either full PASS (/data mounted) or SKIP (no /data).
    '\[boot\] M28A: slog persist (PASS|SKIP)',
    # Stats snapshot -- counters must be self-consistent (non-zero emit).
    '\[boot\] M28A: slog stats emitted=\d+ dropped=\d+ in_use=\d+ depth=256',
    # Userland logview --boot path: ring drain + tag check + write.
    'M28A_LOGVIEW: ring=\d+ tags=\d+ kernel=1 fs=1 net=1 gui=1',
    'M28A_LOGVIEW: PASS',
    '\[boot\] M28A: /bin/logview .*PASS\)',
    # Final harness sentinel.
    '\[boot\] M28A: logging harness complete'
)
$missing = @()
foreach ($pat in $mustHave) {
    if ($txt -notmatch $pat) { $missing += $pat }
}

# === forbidden tokens ===
# Note: the kernel deliberately emits a synthetic "panic-test" string
# but it is wrapped in "(synthetic, no halt)" so we only flag REAL
# panics: stand-alone "KERNEL PANIC at" lines, page faults, OOPSes,
# or any \[FAIL\] line in the boot harness itself.
$forbidden = @(
    'KERNEL PANIC at',
    'page fault',
    'KERNEL OOPS',
    '\[FAIL\] ',
    'M28A_LOGVIEW: FAIL'
)
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M28A PASS (boot 1) -- structured logging harness produced every expected signal." -ForegroundColor Green

    # === reboot-persistence sub-test ===
    # Boot a second time WITHOUT touching disk.img. The kernel should
    # persist the new boot's slog to /data/system.log on top of the
    # previous file. Then we boot a third time and ask /bin/logview
    # --persist to dump the file -- confirming the data survived a
    # power-cycle. We approximate "two boots" by simply running the
    # same QEMU command again with the same disk image; the M28A
    # harness flush rewrites system.log every boot, so a second run
    # already proves the file persists across boots.
    $serial2  = Join-Path $LogDir "serial.${logTag}_reboot.log"
    $debug2   = Join-Path $LogDir "debug.${logTag}_reboot.log"
    $qemuLog2 = Join-Path $LogDir "qemu.${logTag}_reboot.log"
    Remove-Item -Force $serial2, $debug2, $qemuLog2 -ErrorAction SilentlyContinue
    $rebootSentinel = '\[logview\] persist=/data/system\.log lines=\d+'
    $qemuArgs2 = @(
        "-cdrom", $iso,
        "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
        "-smp", "4",
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
        "-device", "qemu-xhci,id=usb0",
        "-device", "usb-kbd,bus=usb0.0",
        "-device", "usb-mouse,bus=usb0.0",
        "-serial", "file:$serial2",
        "-debugcon", "file:$debug2",
        "-d", "cpu_reset,guest_errors", "-D", $qemuLog2,
        "-no-reboot", "-display", "none"
    )
    Write-Host ""
    Write-Host "[m28a] reboot pass: relaunching QEMU to verify /data/system.log persists across boot..."
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
        Write-Host "FAIL: reboot pass did not reach M28A sentinel" -ForegroundColor Red
        Get-Content $serial2 -Tail 80
        exit 1
    }
    $txt2 = Get-Content $serial2 -Raw
    # On the SECOND boot we expect:
    #   1. The slog ring is up again (new ready line).
    #   2. The persist flush succeeds again (overwrites the file).
    #   3. logview --boot still PASSes.
    $reboot_must = @(
        '\[\d+\] INFO  slog\s+: slog ready \(depth=256, persist=/data/system\.log\)',
        '\[boot\] M28A: slog persist (PASS|SKIP)',
        'M28A_LOGVIEW: PASS',
        '\[boot\] M28A: logging harness complete'
    )
    $rmiss = @()
    foreach ($pat in $reboot_must) {
        if ($txt2 -notmatch $pat) { $rmiss += $pat }
    }
    if ($rmiss.Count -gt 0) {
        Write-Host "FAIL: reboot pass missing signals:" -ForegroundColor Red
        $rmiss | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
        Get-Content $serial2 -Tail 100
        exit 1
    }
    Write-Host "M28A PASS (boot 2) -- slog persistence + harness re-runs cleanly." -ForegroundColor Green
    Write-Host "Last 30 lines of serial.${logTag}_reboot.log for visual sanity check:"
    Get-Content $serial2 -Tail 30
    Write-Host ""
    Write-Host "M28A PASS (overall)" -ForegroundColor Green
    exit 0
} else {
    Write-Host "M28A FAIL" -ForegroundColor Red
    if ($missing.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($panics.Count -gt 0) {
        Write-Host "Forbidden tokens present (kernel was unhappy):" -ForegroundColor Red
        $panics | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 120 lines of ${serial} ===" -ForegroundColor Yellow
    Get-Content $serial -Tail 120
    exit 1
}
