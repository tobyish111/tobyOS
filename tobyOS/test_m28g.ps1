# test_m28g.ps1 -- M28G Final Stability Validation Suite.
#
# Strategy: two boots against the same disk image.
#
#   Boot 1 ("stabtest harness"):
#     - Rebuild the initrd with STABTEST_FLAG=1, baking
#       /etc/stabtest_now into the read-only initrd.
#     - Boot QEMU. The kernel m28g_run_stability_harness() runs
#       /bin/stabilitytest --boot followed by --boot --stress.
#     - Verify all 12 SYS_STAB_SELFTEST probes pass (boot, log,
#       panic, watchdog, filesystem, services, gui, terminal,
#       network, input, safe_mode, display) AND the stress run
#       (heap + disk + syscall workload) succeeds.
#
#   Boot 2 ("normal regression"):
#     - Rebuild the initrd WITHOUT STABTEST_FLAG.
#     - Boot QEMU and verify the always-on /bin/stabilitytest --boot
#       still PASSes for all 12 probes. The stress-mode harness
#       must NOT run (gating works), and there must be no panic
#       or service crash on this lightweight pass.
#
# Exit 0 => PASS, exit 1 => FAIL.

$ErrorActionPreference = 'Stop'

$qemu       = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso        = "tobyOS.iso"
$disk       = "disk.img"
$logTag     = "m28g"
$timeoutSec = 90

if (-not (Test-Path $qemu)) {
    Write-Host "qemu not found at $qemu" -ForegroundColor Red; exit 1
}
Set-Location -Path (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Build-Iso {
    param([string]$flag)
    $ttag = if ($flag) { "with STABTEST_FLAG=$flag" } else { "stock" }
    Write-Host "[m28g] make ($ttag) ..." -ForegroundColor Cyan
    if ($flag) {
        & bash -lc "PATH=/c/msys64/ucrt64/bin:`$PATH; cd /c/CustomOS/tobyOS && rm -f build/initrd.tar build/base.iso build/install.img tobyOS.iso && STABTEST_FLAG=$flag make 2>&1 | tail -5"
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
        Write-Host "[m28g] disk.img missing -- running 'make disk.img' to create..." -ForegroundColor Yellow
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
    Write-Host "[m28g] qemu pid=$($proc.Id) waiting for: $endPattern (timeout=${timeoutSec}s)"
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

# Convenience: the M28G probe verdict block has 12 lines; each must say PASS.
function Validate-AllProbesPass {
    param([string]$txt, [string]$boot)
    $expectedProbes = @(
        'boot','log','panic','watchdog','filesystem','services',
        'gui','terminal','network','input','safe_mode','display'
    )
    $bad = @()
    foreach ($p in $expectedProbes) {
        $line = "M28G_STAB: probe=$p verdict=PASS"
        if ($txt -notmatch [regex]::Escape($line)) {
            $bad += "missing or non-PASS for probe '$p' on $boot boot"
        }
    }
    return $bad
}

Write-Host ""
Write-Host "============== M28G: Stability Validation Suite ==============" -ForegroundColor Cyan

# --- Boot 1: stabtest harness (STABTEST_FLAG=1) ---
Build-Iso "1"
Ensure-Disk

$serialA = "serial.${logTag}_stabtest.log"
$debugA  = "debug.${logTag}_stabtest.log"
$qemuA   = "qemu.${logTag}_stabtest.log"
$saw1 = Run-Qemu -serialFile $serialA -debugFile $debugA -qemuFile $qemuA `
                 -endPattern '\[boot\] M28G_STRESS: (PASS|FAIL)' `
                 -timeoutSec $timeoutSec

if (-not (Test-Path $serialA)) {
    Write-Host "FAIL: stabtest boot did not produce $serialA" -ForegroundColor Red
    exit 1
}
$txt1 = Get-Content $serialA -Raw
if (-not $saw1) {
    Write-Host "FAIL: stabtest boot did not surface --stress verdict within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serialA}:" -ForegroundColor Yellow
    Get-Content $serialA -Tail 80
    exit 1
}

# Required signals on the STABTEST_FLAG=1 boot.
$reqA = @(
    # Always-on lightweight self-test fires first.
    '\[boot\] M28G: /bin/stabilitytest --boot pid=\d+ exit=0 \(PASS\)',
    'M28G_STAB: PASS pass=12 fail=0',
    # Gated stress harness fires.
    '\[boot\] M28G: /etc/stabtest_now present -- running stabilitytest --stress',
    '\[boot\] M28G_STRESS: PASS'
)

# Forbidden tokens on the stabtest boot.
$badA = @(
    'KERNEL PANIC at',
    'KERNEL OOPS',
    'M28G_STAB: FAIL',
    '\[boot\] M28G_STRESS: FAIL'
)

$missingA = Validate-AllProbesPass -txt $txt1 -boot 'stabtest'
foreach ($pat in $reqA) {
    if ($txt1 -notmatch $pat) { $missingA += $pat }
}
$forbiddenA = @()
foreach ($pat in $badA) {
    if ($txt1 -match $pat) { $forbiddenA += $pat }
}

if ($missingA.Count -gt 0 -or $forbiddenA.Count -gt 0) {
    Write-Host "M28G FAIL (stabtest boot)" -ForegroundColor Red
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
Write-Host "M28G PASS (stabtest boot) -- 12/12 probes PASS, stress workload PASS." -ForegroundColor Green

# --- Boot 2: normal regression ---
Build-Iso ""
$serialB = "serial.${logTag}_normal.log"
$debugB  = "debug.${logTag}_normal.log"
$qemuB   = "qemu.${logTag}_normal.log"
$saw2 = Run-Qemu -serialFile $serialB -debugFile $debugB -qemuFile $qemuB `
                 -endPattern '\[boot\] M28G: /bin/stabilitytest --boot pid=\d+ exit=' `
                 -timeoutSec $timeoutSec

if (-not (Test-Path $serialB)) {
    Write-Host "FAIL: normal boot did not produce $serialB" -ForegroundColor Red
    exit 1
}
$txt2 = Get-Content $serialB -Raw
if (-not $saw2) {
    Write-Host "FAIL: normal boot did not reach M28G live-stabilitytest verdict within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serialB}:" -ForegroundColor Yellow
    Get-Content $serialB -Tail 80
    exit 1
}

$reqB = @(
    '\[boot\] M28G: /bin/stabilitytest --boot pid=\d+ exit=0 \(PASS\)',
    'M28G_STAB: PASS pass=12 fail=0'
)
$badB = @(
    'KERNEL PANIC at',
    'KERNEL OOPS',
    'M28G_STAB: FAIL',
    # Gated stress test must NOT have run on the normal boot.
    '\[boot\] M28G: /etc/stabtest_now present',
    '\[boot\] M28G_STRESS:'
)

$missingB = Validate-AllProbesPass -txt $txt2 -boot 'normal'
foreach ($pat in $reqB) {
    if ($txt2 -notmatch $pat) { $missingB += $pat }
}
$forbiddenB = @()
foreach ($pat in $badB) {
    if ($txt2 -match $pat) { $forbiddenB += $pat }
}
if ($missingB.Count -gt 0 -or $forbiddenB.Count -gt 0) {
    Write-Host "M28G FAIL (normal regression)" -ForegroundColor Red
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

Write-Host "M28G PASS (normal regression) -- 12/12 probes PASS, gated stress absent." -ForegroundColor Green
Write-Host ""
Write-Host "=== M28G probe details from stabtest boot ===" -ForegroundColor DarkGray
([regex]::Matches($txt1, "M28G_STAB: [^\r\n]+") | ForEach-Object { $_.Value }) -join "`r`n"
Write-Host ""
Write-Host "M28G PASS (overall)" -ForegroundColor Green
exit 0
