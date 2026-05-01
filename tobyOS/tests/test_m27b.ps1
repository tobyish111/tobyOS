# test_m27b.ps1 -- M27B Rendering Backend Cleanup validation.
#
# M27B extended the gfx_backend struct with present_rect, describe and
# bytes_per_pixel slots, refactored gfx.c to drive a dirty-rectangle
# accumulator, and added explicit out-of-bounds defenses around fill_rect
# and blit. The default Limine-fb backend now implements all four ABI
# fields; the virtio-gpu backend implements three (present_rect=NULL).
#
# Validation:
#   - same boot harness as M27A drives /bin/displayinfo, /bin/drawtest,
#     /bin/rendertest -- so we re-use the M27A regex set + add new ones
#     specific to M27B (kernel-side display_render reports backend
#     capability tags; new userland 'rendertest backend' case asserts
#     the kernel name<->id mapping is consistent).
#
# The per-phase script exists so a regression in M27B's refactor is
# attributed correctly even when M27A's surface-level signals still
# pass.


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m27b"
$serial   = Join-Path $LogDir "serial.$logTag.log"
$debug    = Join-Path $LogDir "debug.$logTag.log"
$qemuLog  = Join-Path $LogDir "qemu.$logTag.log"
$bootSentinel = '\[boot\] M27A: display .* PASS .* of \d+'
$timeoutSec   = 45

if (-not (Test-Path $iso))  { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M27B: Rendering Backend Cleanup ==============" -ForegroundColor Cyan

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
Write-Host "[m27b] qemu pid = $($proc.Id), watching $serial for boot sentinel..."

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

# === required signals (M27B-specific) ===
$mustHave = @(
    # devtest_boot_run() output for the kernel-side display tests.
    # display_render now reports the backend capability tags (M27B).
    'display_render:.*backend=limine-fb',
    'display_render:.*flip=ok',
    'display_render:.*present_rect=yes',
    'display_render:.*describe=yes',
    # display_dirty went from SKIP to PASS now that the dirty-rect
    # tracker is wired up.
    '\[PASS\] display_dirty: tracker OK',
    # New userland rendertest backend case (M27B).
    '\[PASS\] rendertest backend.*backend=limine-fb id=1 \(mapped=limine-fb\)',
    # No bounds-check violation should fire on a clean boot. drawtest
    # exercises clipping; if a violation slipped through we'd see this
    # line. Treat its absence as a positive signal (no-string-match).
    # (We still grep mustNotHave for it below.)
    # M27A baseline still passes.
    '\[boot\] M27A: display \d+ PASS / 0 FAIL / 0 missing of \d+'
)
$missing = @()
foreach ($pat in $mustHave) {
    if ($txt -notmatch $pat) { $missing += $pat }
}

# === forbidden tokens (must NOT appear) ===
$forbidden = @(
    'panic',
    'page fault',
    'KERNEL OOPS',
    '\[FAIL\] ',
    # M27B added these defensive log lines; they should NEVER fire on
    # a clean boot. Their presence indicates a clipping bug.
    '\[gfx\] fill_rect bounds violation',
    '\[gfx\] blit bounds violation'
)
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M27B PASS -- backend ABI extended, dirty-rect tracker live, bounds checks silent." -ForegroundColor Green
    Write-Host "Last 30 lines of serial.$logTag.log for visual sanity check:"
    Get-Content $serial -Tail 30
    exit 0
} else {
    Write-Host "M27B FAIL" -ForegroundColor Red
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
