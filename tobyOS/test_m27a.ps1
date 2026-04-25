# test_m27a.ps1 -- M27A Display Test Harness validation.
#
# Drives the boot-time M27A pipeline (kernel.c::m27a_run_userland_tools).
# That pipeline spawns:
#   /bin/displayinfo
#   /bin/displayinfo --json
#   /bin/drawtest
#   /bin/rendertest
# and prints `[boot] M27A: ... PASS` lines to serial.
#
# This script just boots tobyOS in QEMU, waits for the M27A summary line,
# kills QEMU, and verifies the captured serial.log contains every expected
# signal. Exit 0 => PASS, exit 1 => FAIL (with a list of missing patterns).

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m27a"
$serial   = "serial.$logTag.log"
$debug    = "debug.$logTag.log"
$qemuLog  = "qemu.$logTag.log"
$bootSentinel = '\[boot\] M27A: display .* PASS .* of \d+'
$timeoutSec   = 45

if (-not (Test-Path $iso))  { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M27A: Display Test Harness ==============" -ForegroundColor Cyan

# Same QEMU args as M26A so the boot path is identical -- we only care
# about the display tools producing PASS lines, not about USB/audio.
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
Write-Host "[m27a] qemu pid = $($proc.Id), watching $serial for boot sentinel..."

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
    # Kernel-side display registry must come up.
    '\[display\] registered fb0 \d+x\d+ backend=',
    # Boot-time M27A spawn lines for every tool.
    '\[boot\] M27A: driving display test harness',
    '\[boot\] M27A: /bin/displayinfo .*PASS\)',
    '\[boot\] M27A: /bin/drawtest .*PASS\)',
    '\[boot\] M27A: /bin/rendertest .*PASS\)',
    # Userland output -- confirms the table renderer ran. The ORIGIN
    # column was added in M27G; pre-M27G test runs would have matched
    # the old "FORMAT FLAGS" pair, but post-M27G the header is
    # "FORMAT ORIGIN FLAGS". Accept either so the script can run
    # against either layout without modification.
    'IDX\s+NAME\s+BACKEND\s+RES\s+BPP\s+PITCH\s+FORMAT(\s+ORIGIN)?\s+FLAGS',
    'PASS: displayinfo: \d+ output\(s\)',
    # drawtest summary (primitive count is dynamic but printed verbatim).
    'PASS: drawtest: \d+ primitive\(s\) ran cleanly',
    # rendertest cases that should always run.
    '\[PASS\] rendertest basic',
    '\[PASS\] rendertest geometry',
    '\[PASS\] rendertest primitives',
    '\[PASS\] rendertest overlap',
    # cursor is SKIPped at boot harness time (compositor hasn't
    # produced frames yet -- flips==0). It re-runs interactively
    # from the shell where it has teeth.
    '\[(PASS|SKIP)\] rendertest cursor',
    '\[PASS\] rendertest backend',
    # M27C wired alpha; M27D wired font (still SKIP at userland-only); M27E wired dirty.
    '\[(PASS|SKIP)\] rendertest alpha',
    '\[(PASS|SKIP)\] rendertest font',
    '\[(PASS|SKIP)\] rendertest dirty',
    'PASS: rendertest: pass=\d+ skip=\d+ fail=0',
    # Overall harness summary -- self-referencing regex so we keep saying
    # "every tool passed" without hard-coding the count.
    '\[boot\] M27A: display (\d+) PASS / 0 FAIL / 0 missing of \1'
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
    Write-Host "M27A PASS -- display harness boot sweep + every userland tool produced expected output." -ForegroundColor Green
    Write-Host "Last 40 lines of serial.$logTag.log for visual sanity check:"
    Get-Content $serial -Tail 40
    exit 0
} else {
    Write-Host "M27A FAIL" -ForegroundColor Red
    if ($missing.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($panics.Count -gt 0) {
        Write-Host "Forbidden tokens present (kernel was unhappy):" -ForegroundColor Red
        $panics | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 100 lines of ${serial} ===" -ForegroundColor Yellow
    Get-Content $serial -Tail 100
    exit 1
}
