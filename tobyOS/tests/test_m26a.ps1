# test_m26a.ps1 -- M26A Peripheral Test Harness validation.
#
# The harness is self-driven from kernel.c: every boot already runs
#   1. devtest_boot_run()           (kernel-side inventory + selftests)
#   2. m26a_run_userland_tools()    (shell builtins + /bin/{devlist,
#      drvtest,usbtest,audiotest,batterytest} via proc_spawn)
#
# So this script just boots tobyOS in QEMU, lets the boot harness run,
# kills QEMU once the validation lines have appeared, and greps the
# captured serial.log for the required PASS / SKIP signal set.
#
# Exit 0 => PASS, exit 1 => FAIL (with a list of missing patterns).


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m26a"
$serial   = Join-Path $LogDir "serial.$logTag.log"
$debug    = Join-Path $LogDir "debug.$logTag.log"
$qemuLog  = Join-Path $LogDir "qemu.$logTag.log"
$bootSentinel = '\[boot\] M26A: userland .* PASS .* of \d+'
$timeoutSec   = 30

if (-not (Test-Path $iso))  { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M26A: Peripheral Test Harness ==============" -ForegroundColor Cyan

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
Write-Host "[m26a] qemu pid = $($proc.Id), watching $serial for boot sentinel..."

# Poll serial.log until the harness sentinel shows up, or we hit the
# timeout. As soon as the sentinel appears (or panic shows up), kill QEMU.
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
    Get-Content $serial -Tail 60
    exit 1
}

# === required signals ===
$mustHave = @(
    # Kernel-side inventory + selftest sweep.
    '\[boot\] M26A: peripheral inventory \+ self-tests',
    '\[boot\] M26A: \d+ test\(s\) -- pass=\d+ fail=\d+ skip=\d+',
    '\[PASS\] devtest:',
    '\[PASS\] pci:',
    '\[PASS\] xhci:',
    '\[SKIP\] audio:',
    '\[SKIP\] battery:',
    # Shell-side builtins (devlist + drvtest exercised through shell_run_test_line).
    '\[boot\] M26A: driving shell builtins \(devlist \+ drvtest\)',
    # Userland binaries (each spawned + waited for, exit 0 expected).
    '\[boot\] M26A: /bin/devlist .*PASS\)',
    '\[boot\] M26A: /bin/drvtest .*PASS\)',
    '\[boot\] M26A: /bin/usbtest .*PASS\)',
    '\[boot\] M26A: /bin/audiotest .*PASS\)',
    '\[boot\] M26A: /bin/batterytest .*PASS\)',
    # Userland output formatting.
    'BUS\s+NAME\s+DRIVER\s+STAT\s+INFO',
    'PASS: devlist: \d+ device\(s\)',
    'PASS: drvtest: pass=\d+ skip=\d+ fail=\d+',
    'PASS: usbtest list: \d+ USB device\(s\)',
    'PASS: usbtest controller: xHCI',
    'PASS: usbtest devices:',
    # Final summary -- every queued tool must spawn and exit 0. The
    # exact count grows with later phases (M26B added 2, M26C/D/E will
    # add more); use a self-referencing regex so we keep asserting
    # "every tool passed" without hard-coding the number.
    '\[boot\] M26A: userland (\d+) PASS / 0 FAIL / 0 missing of \1'
)
$missing = @()
foreach ($pat in $mustHave) {
    if ($txt -notmatch $pat) { $missing += $pat }
}

# === forbidden tokens ===
$forbidden = @('panic', 'page fault', 'KERNEL OOPS', '\[FAIL\] ')
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M26A PASS -- harness boot sweep + every userland tool produced expected output." -ForegroundColor Green
    Write-Host "Last 30 lines of serial.$logTag.log for visual sanity check:"
    Get-Content $serial -Tail 30
    exit 0
} else {
    Write-Host "M26A FAIL" -ForegroundColor Red
    if ($missing.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($panics.Count -gt 0) {
        Write-Host "Forbidden tokens present (kernel was unhappy):" -ForegroundColor Red
        $panics | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 80 lines of ${serial} ===" -ForegroundColor Yellow
    Get-Content $serial -Tail 80
    exit 1
}
