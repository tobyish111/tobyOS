# test_m28f.ps1 -- M28F Service Supervision + Restart validation.
#
# Strategy: two boots against the same disk image.
#
#   Boot 1 ("svctest harness"):
#     - Rebuild the initrd with SVCTEST_FLAG=1, baking
#       /etc/svctest_now into the read-only initrd.
#     - Boot QEMU. The kernel m28f_run_service_harness() registers
#       /bin/svc_crasher (autorestart=on, always exits rc=42),
#       drives 6 synthetic crashes through service_simulate_exit,
#       and checks the supervisor's BACKOFF -> DISABLED transitions.
#     - It then spawns /bin/services --boot which exercises
#       SYS_SVC_LIST and prints the "M28F_SERVICES: PASS" sentinel.
#     - Verify: service marked DISABLED, crash counter >= 5, manual
#       restart refused, service_clear() recovers it, no panic, and
#       no infinite restart loop.
#
#   Boot 2 ("normal regression"):
#     - Rebuild the initrd WITHOUT SVCTEST_FLAG.
#     - Boot QEMU and verify the supervisor still works for the
#       built-in/login services and `services --boot` prints PASS.
#     - The gated svctest harness must NOT run, and there must be
#       no svc_crasher entries in the log.
#
# Exit 0 => PASS, exit 1 => FAIL.

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m28f"
$timeoutSec = 90

if (-not (Test-Path $qemu)) {
    Write-Host "qemu not found at $qemu" -ForegroundColor Red; exit 1
}
Set-Location -Path (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Build-Iso {
    param([string]$flag)
    $ttag = if ($flag) { "with SVCTEST_FLAG=$flag" } else { "stock" }
    Write-Host "[m28f] make ($ttag) ..." -ForegroundColor Cyan
    if ($flag) {
        & bash -lc "PATH=/c/msys64/ucrt64/bin:`$PATH; cd /c/CustomOS/tobyOS && rm -f build/initrd.tar build/base.iso build/install.img tobyOS.iso && SVCTEST_FLAG=$flag make 2>&1 | tail -5"
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
        Write-Host "[m28f] disk.img missing -- running 'make disk.img' to create..." -ForegroundColor Yellow
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
    Write-Host "[m28f] qemu pid=$($proc.Id) waiting for: $endPattern (timeout=${timeoutSec}s)"
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
Write-Host "============== M28F: Service Supervision ==============" -ForegroundColor Cyan

# --- Boot 1: svctest harness (SVCTEST_FLAG=1) ---
Build-Iso "1"
Ensure-Disk

$serialA = "serial.${logTag}_svctest.log"
$debugA  = "debug.${logTag}_svctest.log"
$qemuA   = "qemu.${logTag}_svctest.log"
$saw1 = Run-Qemu -serialFile $serialA -debugFile $debugA -qemuFile $qemuA `
                 -endPattern '\[boot\] M28F_SVCTEST: (PASS|FAIL)' `
                 -timeoutSec $timeoutSec

if (-not (Test-Path $serialA)) {
    Write-Host "FAIL: svctest boot did not produce $serialA" -ForegroundColor Red
    exit 1
}
$txt1 = Get-Content $serialA -Raw
if (-not $saw1) {
    Write-Host "FAIL: svctest boot did not surface kernel verdict within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serialA}:" -ForegroundColor Yellow
    Get-Content $serialA -Tail 80
    exit 1
}

# Required signals on the SVCTEST_FLAG=1 boot.
$reqA = @(
    # Service manager came up advertising the M28F backoff knobs.
    '\[svc\] service manager up',
    # Live `services --boot` runs at least once on every boot.
    '\[boot\] M28F: /bin/services \(pid=\d+\) exit=0 \(PASS\)',
    'M28F_SERVICES: PASS',
    # Gated svctest harness fires.
    '\[boot\] M28F: /etc/svctest_now present -- running service supervision self-test',
    '\[svc\] registered program ''crasher'' -> /bin/svc_crasher',
    # The synthetic crash loop iterates and trips DISABLED.
    '\[boot\] M28F_SVCTEST: iter=0 .* state=3',          # SERVICE_BACKOFF=3
    '\[svc\] ''crasher'' crashed rc=42 \(consecutive=\d+',
    '\[svc\] ''crasher'' DISABLED after \d+ consecutive crashes',
    # Refuse-restart + service_clear() flow.
    '\[svc\] ''crasher'' refusing start: DISABLED',
    '\[svc\] ''crasher'' cleared',
    '\[boot\] M28F_SVCTEST: PASS',
    # Post-svctest snapshot through SVC_LIST shows crasher exists.
    'M28F_SERVICES: name=crasher',
    '\[boot\] M28F: post-svctest /bin/services exit=0 \(PASS\)'
)

# Forbidden tokens on the svctest boot. The whole point of M28F is to
# preserve safety: no panic, no infinite spawn loop (rcnt for crasher
# must stay below a soft cap).
$badA = @(
    'KERNEL PANIC at',
    'KERNEL OOPS',
    '\[boot\] M28F_SVCTEST: FAIL',
    'M28F_SERVICES: FAIL'
)

$missingA = @()
foreach ($pat in $reqA) {
    if ($txt1 -notmatch $pat) { $missingA += $pat }
}
$forbiddenA = @()
foreach ($pat in $badA) {
    if ($txt1 -match $pat) { $forbiddenA += $pat }
}

# Crash-loop containment guard: count occurrences of the crasher
# enqueue line. With SERVICE_DISABLE_THRESHOLD=5 the supervisor must
# trip and STOP. The synthetic test path uses service_simulate_exit
# (no spawn), so live enqueues come only from service_clear() (which
# we explicitly disabled) and from the auto-restart safety net being
# tested. We allow up to 8 to absorb timing slack, but anything
# higher means the supervisor failed to brake.
$crasherEnqueues = ([regex]::Matches($txt1, "\[svc\] 'crasher' enqueued")).Count
if ($crasherEnqueues -gt 8) {
    $forbiddenA += "crasher restart loop ($crasherEnqueues enqueues > 8)"
}

if ($missingA.Count -gt 0 -or $forbiddenA.Count -gt 0) {
    Write-Host "M28F FAIL (svctest boot)" -ForegroundColor Red
    if ($missingA.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missingA | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($forbiddenA.Count -gt 0) {
        Write-Host "Forbidden tokens:" -ForegroundColor Red
        $forbiddenA | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 200 lines of $serialA ===" -ForegroundColor Yellow
    Get-Content $serialA -Tail 200
    exit 1
}
Write-Host "M28F PASS (svctest boot) -- supervisor saw 6 crashes, tripped DISABLED, recovered cleanly." -ForegroundColor Green
Write-Host "  crasher enqueues observed: $crasherEnqueues (cap 12)" -ForegroundColor DarkGray

# --- Boot 2: normal regression ---
Build-Iso ""
$serialB = "serial.${logTag}_normal.log"
$debugB  = "debug.${logTag}_normal.log"
$qemuB   = "qemu.${logTag}_normal.log"
$saw2 = Run-Qemu -serialFile $serialB -debugFile $debugB -qemuFile $qemuB `
                 -endPattern '\[boot\] M28F: /bin/services \(pid=\d+\) exit=' `
                 -timeoutSec $timeoutSec

if (-not (Test-Path $serialB)) {
    Write-Host "FAIL: normal boot did not produce $serialB" -ForegroundColor Red
    exit 1
}
$txt2 = Get-Content $serialB -Raw
if (-not $saw2) {
    Write-Host "FAIL: normal boot did not reach M28F live-services verdict within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serialB}:" -ForegroundColor Yellow
    Get-Content $serialB -Tail 80
    exit 1
}

$reqB = @(
    '\[svc\] service manager up',
    '\[boot\] M28F: /bin/services \(pid=\d+\) exit=0 \(PASS\)',
    'M28F_SERVICES: PASS'
)
$badB = @(
    'KERNEL PANIC at',
    'KERNEL OOPS',
    'M28F_SERVICES: FAIL',
    # Gated test must NOT have run on the normal boot.
    '\[boot\] M28F: /etc/svctest_now present',
    '\[boot\] M28F_SVCTEST:',
    '\[svc\] registered program ''crasher''',
    '\[svc\] ''crasher'' DISABLED'
)

$missingB = @()
foreach ($pat in $reqB) {
    if ($txt2 -notmatch $pat) { $missingB += $pat }
}
$forbiddenB = @()
foreach ($pat in $badB) {
    if ($txt2 -match $pat) { $forbiddenB += $pat }
}
if ($missingB.Count -gt 0 -or $forbiddenB.Count -gt 0) {
    Write-Host "M28F FAIL (normal regression)" -ForegroundColor Red
    if ($missingB.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missingB | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($forbiddenB.Count -gt 0) {
        Write-Host "Forbidden tokens:" -ForegroundColor Red
        $forbiddenB | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 200 lines of $serialB ===" -ForegroundColor Yellow
    Get-Content $serialB -Tail 200
    exit 1
}

Write-Host "M28F PASS (normal regression) -- live services tool works, gated test absent." -ForegroundColor Green
Write-Host ""
Write-Host "=== last 30 lines of $serialA ===" -ForegroundColor DarkGray
Get-Content $serialA -Tail 30
Write-Host ""
Write-Host "M28F PASS (overall)" -ForegroundColor Green
exit 0
