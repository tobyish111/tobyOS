# test_m28e.ps1 -- M28E Filesystem Integrity + Recovery validation.
#
# Strategy: two boots against the same disk image.
#
#   Boot 1 ("fscheck harness"):
#     - Rebuild the initrd with FSCHECK_FLAG=1, baking
#       /etc/fscheck_now into the read-only initrd.
#     - Boot QEMU. tobyfs_mount() runs check_core() before mounting
#       /data; on a normal disk image it must report clean (OK).
#     - The boot harness then:
#         (a) spawns /bin/fscheck --boot /data, which exercises
#             SYS_FS_CHECK end-to-end and prints "M28E_FSCHECK: PASS",
#         (b) calls tobyfs_self_test which builds a 4 MiB ramdisk,
#             formats it (clean -> TFS_CHECK_OK), then stomps the
#             magic and re-checks (corrupt -> TFS_CHECK_FATAL), and
#             prints "M28E_KERNEL_FSCHECK: PASS".
#     - Verify the serial log proves both userland and kernel-side
#       integrity paths fired, AND that no kernel panic occurred
#       (corruption is detected with a "safe error", not a crash).
#
#   Boot 2 ("normal regression"):
#     - Rebuild the initrd WITHOUT FSCHECK_FLAG.
#     - Boot QEMU and verify:
#         * /data still mounts clean,
#         * the live /bin/fscheck --boot run still succeeds,
#         * the gated kernel self-test does NOT run,
#         * no panic / oops anywhere.
#
# Exit 0 => PASS, exit 1 => FAIL.

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m28e"
$timeoutSec = 90

if (-not (Test-Path $qemu)) {
    Write-Host "qemu not found at $qemu" -ForegroundColor Red; exit 1
}
Set-Location -Path (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Build-Iso {
    param([string]$flag)
    $ttag = if ($flag) { "with FSCHECK_FLAG=$flag" } else { "stock" }
    Write-Host "[m28e] make ($ttag) ..." -ForegroundColor Cyan
    if ($flag) {
        & bash -lc "PATH=/c/msys64/ucrt64/bin:`$PATH; cd /c/CustomOS/tobyOS && rm -f build/initrd.tar build/base.iso build/install.img tobyOS.iso && FSCHECK_FLAG=$flag make 2>&1 | tail -5"
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
        Write-Host "[m28e] disk.img missing -- running 'make disk.img' to create..." -ForegroundColor Yellow
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
    Write-Host "[m28e] qemu pid=$($proc.Id) waiting for: $endPattern (timeout=${timeoutSec}s)"
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
Write-Host "============== M28E: Filesystem Integrity ==============" -ForegroundColor Cyan

# --- Boot 1: fscheck harness (FSCHECK_FLAG=1) ---
Build-Iso "1"
Ensure-Disk

$serialA = "serial.${logTag}_check.log"
$debugA  = "debug.${logTag}_check.log"
$qemuA   = "qemu.${logTag}_check.log"
$saw1 = Run-Qemu -serialFile $serialA -debugFile $debugA -qemuFile $qemuA `
                 -endPattern 'M28E_KERNEL_FSCHECK: (PASS|FAIL)' `
                 -timeoutSec $timeoutSec

if (-not (Test-Path $serialA)) {
    Write-Host "FAIL: fscheck boot did not produce $serialA" -ForegroundColor Red
    exit 1
}
$txt1 = Get-Content $serialA -Raw
if (-not $saw1) {
    Write-Host "FAIL: fscheck boot did not surface kernel verdict within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serialA}:" -ForegroundColor Yellow
    Get-Content $serialA -Tail 80
    exit 1
}

# Required signals on the FSCHECK_FLAG=1 boot.
$reqA = @(
    # /data mounted with the new mount-time integrity gate.
    '\[tobyfs\] mount of ''/data'' clean: inodes=\d+/\d+ blocks=\d+/\d+',
    # Live userland fscheck on /data.
    '\[boot\] M28E: /data mounted -- spawning /bin/fscheck --boot',
    'M28E_FSCHECK: path=/data fs_type=tobyfs status=OK',
    'M28E_FSCHECK: PASS',
    '\[boot\] M28E: /bin/fscheck \(pid=\d+\) exit=0 \(PASS\)',
    # Kernel-side ramdisk corruption-detection self-test.
    '\[boot\] M28E: /etc/fscheck_now present -- running kernel corruption-detection self-test',
    '\[m28e\] self-test: ramdev formatted',
    '\[m28e\] self-test: clean check rc=0 sev=0 errors=0',
    # check_core's detail string for bad magic; allow either decimal or hex format
    '\[m28e\] self-test: corrupt check rc=0 sev=2 errors=\d+ detail="bad superblock magic',
    '\[boot\] M28E_KERNEL_FSCHECK: clean_sev=0 clean_errors=0 corrupt_sev=2',
    '\[boot\] M28E_KERNEL_FSCHECK: PASS'
)

# Forbidden tokens on the fscheck boot. Most importantly: corruption
# detection MUST NOT crash the kernel.
$badA = @(
    'KERNEL PANIC at',
    'KERNEL OOPS',
    'M28E_FSCHECK: FAIL',
    'M28E_KERNEL_FSCHECK: FAIL'
)

$missingA = @()
foreach ($pat in $reqA) {
    if ($txt1 -notmatch $pat) { $missingA += $pat }
}
$forbiddenA = @()
foreach ($pat in $badA) {
    if ($txt1 -match $pat) { $forbiddenA += $pat }
}

if ($missingA.Count -gt 0 -or $forbiddenA.Count -gt 0) {
    Write-Host "M28E FAIL (fscheck boot)" -ForegroundColor Red
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
Write-Host "M28E PASS (fscheck boot) -- live + kernel corruption-detection both fired, no crash." -ForegroundColor Green

# --- Boot 2: normal regression ---
Build-Iso ""
$serialB = "serial.${logTag}_normal.log"
$debugB  = "debug.${logTag}_normal.log"
$qemuB   = "qemu.${logTag}_normal.log"
$saw2 = Run-Qemu -serialFile $serialB -debugFile $debugB -qemuFile $qemuB `
                 -endPattern '\[boot\] M28E: /bin/fscheck \(pid=\d+\) exit=' `
                 -timeoutSec $timeoutSec

if (-not (Test-Path $serialB)) {
    Write-Host "FAIL: normal boot did not produce $serialB" -ForegroundColor Red
    exit 1
}
$txt2 = Get-Content $serialB -Raw
if (-not $saw2) {
    Write-Host "FAIL: normal boot did not reach M28E live-fscheck verdict within ${timeoutSec}s" -ForegroundColor Red
    Write-Host "Tail of ${serialB}:" -ForegroundColor Yellow
    Get-Content $serialB -Tail 80
    exit 1
}

$reqB = @(
    # /data mount-time integrity gate still passes.
    '\[tobyfs\] mount of ''/data'' clean: inodes=\d+/\d+ blocks=\d+/\d+',
    # Userland live probe.
    '\[boot\] M28E: /data mounted -- spawning /bin/fscheck --boot',
    'M28E_FSCHECK: PASS',
    '\[boot\] M28E: /bin/fscheck \(pid=\d+\) exit=0 \(PASS\)'
)
$badB = @(
    'KERNEL PANIC at',
    'KERNEL OOPS',
    'M28E_FSCHECK: FAIL',
    # Gated test must NOT have run on the normal boot.
    '\[boot\] M28E: /etc/fscheck_now present',
    '\[m28e\] self-test:',
    'M28E_KERNEL_FSCHECK:'
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
    Write-Host "M28E FAIL (normal regression)" -ForegroundColor Red
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

Write-Host "M28E PASS (normal regression) -- live fscheck still works, no gated test leakage." -ForegroundColor Green
Write-Host ""
Write-Host "=== last 30 lines of $serialA ===" -ForegroundColor DarkGray
Get-Content $serialA -Tail 30
Write-Host ""
Write-Host "M28E PASS (overall)" -ForegroundColor Green
exit 0
