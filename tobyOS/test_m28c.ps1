# test_m28c.ps1 -- M28C Watchdog + Hang Detection validation.
#
# Strategy: single boot with WDOGTEST_FLAG=1.
#
#   - Rebuild the initrd with WDOGTEST_FLAG=1, baking /etc/wdogtest_now
#     into the read-only initrd.
#   - Boot QEMU. After M28A logging + M28B crashinfo inspector run,
#     kernel.c sees /etc/wdogtest_now exists. It:
#       * lowers the watchdog timeout to 600 ms,
#       * calls wdog_simulate_kernel_stall(1500) -- kernel context busy
#         loop with sched heartbeat frozen, PIT IRQs continue to fire,
#         wdog_check() runs from PIT IRQ at 1 Hz and detects the stale
#         sched heartbeat, fires a sched_stall bite event,
#       * spawns /bin/wdogtest --boot which reads SYS_WDOG_STATUS and
#         prints "M28C_WDOG: PASS" if event_count >= 1 and the last
#         event kind is sched_stall.
#   - Verify the serial log contains:
#       * the kernel-side wdog log lines,
#       * the bite event log line,
#       * the userland PASS sentinel.
#   - Verify the system did NOT panic (the watchdog fired but recovered).
#
# Exit 0 => PASS, exit 1 => FAIL.

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m28c"
$serial   = "serial.$logTag.log"
$debug    = "debug.$logTag.log"
$qemuLog  = "qemu.$logTag.log"
$timeoutSec = 90

# Sentinels that mark the end of the M28C harness boot. The userland
# tool always prints either PASS or FAIL.
$endSentinel = 'M28C_WDOG: PASS|M28C_WDOG: FAIL'

if (-not (Test-Path $qemu)) {
    Write-Host "qemu not found at $qemu" -ForegroundColor Red; exit 1
}

# The script lives in c:\CustomOS\tobyOS\ -- run from there.
Set-Location -Path (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Build-Iso {
    param([string]$flag)
    $ttag = if ($flag) { "with WDOGTEST_FLAG=$flag" } else { "stock" }
    Write-Host "[m28c] make ($ttag) ..." -ForegroundColor Cyan
    if ($flag) {
        & bash -lc "PATH=/c/msys64/ucrt64/bin:`$PATH; cd /c/CustomOS/tobyOS && rm -f build/initrd.tar build/base.iso build/install.img tobyOS.iso && WDOGTEST_FLAG=$flag make 2>&1 | tail -5"
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
        Write-Host "[m28c] disk.img missing -- running 'make disk.img' to create..." -ForegroundColor Yellow
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
    Write-Host "[m28c] qemu pid=$($proc.Id) waiting for: $endPattern (timeout=${timeoutSec}s)"
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
Write-Host "============== M28C: Watchdog + Hang Detection ==============" -ForegroundColor Cyan

Build-Iso "1"
Ensure-Disk

$saw = Run-Qemu -serialFile $serial -debugFile $debug -qemuFile $qemuLog `
                -endPattern $endSentinel -timeoutSec $timeoutSec

if (-not (Test-Path $serial)) {
    Write-Host "FAIL: wdog boot did not produce $serial" -ForegroundColor Red
    exit 1
}
$txt = Get-Content $serial -Raw
if (-not $saw) {
    Write-Host "FAIL: wdog boot did not surface wdogtest verdict within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serial}:" -ForegroundColor Yellow
    Get-Content $serial -Tail 80
    exit 1
}

# Required sentinels:
#   - watchdog ready line at boot
#   - the boot harness saw the flag and dropped the timeout
#   - the simulated stall ran
#   - wdog_check fired and recorded a bite event (kernel-side log line)
#   - the userland tool spawned, read SYS_WDOG_STATUS, printed PASS
#   - kernel survived -- no panic banner
$required = @(
    # Boot order: watchdog initialises BEFORE pit_init.
    '\[wdog\] ready \(timeout=10000 ms\)|wdog.*watchdog ready',
    # Harness sees flag.
    '\[boot\] M28C: /etc/wdogtest_now present -- running watchdog hang harness',
    # Timeout reduced for the test.
    '\[boot\] M28C: timeout reduced to 600 ms',
    # The simulated stall actually runs.
    '\[boot\] M28C: simulating 1500 ms kernel stall',
    # The watchdog detected the stall (kprintf line in wdog_record_event).
    "\[wdog\] BITE event=\d+ kind=sched_stall pid=-1",
    # The harness reaches the spawn line.
    '\[boot\] M28C: stall complete; spawning /bin/wdogtest --boot',
    # Userland sees the bite via the syscall.
    'M28C_WDOG: enabled=1 timeout_ms=600 ',
    'M28C_WDOG: reason="scheduler heartbeat stalled"',
    'M28C_WDOG: PASS',
    # The boot harness logs the userland exit code.
    '\[boot\] M28C: /bin/wdogtest \(pid=\d+\) exit=0 \(PASS\)',
    # Restore pass.
    '\[boot\] M28C: timeout restored to 10000 ms',
    '\[boot\] M28C: watchdog harness complete'
)

# After the bite the system MUST recover -- no kernel panic, no oops.
$forbidden = @(
    'KERNEL PANIC at',
    'KERNEL OOPS',
    'page fault',
    'M28C_WDOG: FAIL'
)

$missing = @()
foreach ($pat in $required) {
    if ($txt -notmatch $pat) { $missing += $pat }
}
$bad = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $bad += $pat }
}

if ($missing.Count -gt 0 -or $bad.Count -gt 0) {
    Write-Host "M28C FAIL" -ForegroundColor Red
    if ($missing.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($bad.Count -gt 0) {
        Write-Host "Forbidden tokens (system was supposed to recover):" -ForegroundColor Red
        $bad | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 200 lines of $serial ===" -ForegroundColor Yellow
    Get-Content $serial -Tail 200
    exit 1
}

Write-Host "M28C PASS -- watchdog detected sched stall, logged bite event, system recovered." -ForegroundColor Green
Write-Host ""
Write-Host "=== last 40 lines of $serial ===" -ForegroundColor DarkGray
Get-Content $serial -Tail 40
Write-Host ""
Write-Host "M28C PASS (overall)" -ForegroundColor Green
exit 0
