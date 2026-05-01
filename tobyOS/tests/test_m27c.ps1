# test_m27c.ps1 -- M27C Alpha Blending + Better Compositing.
#
# What M27C added:
#   - kernel-side gfx_blend_pixel_argb (rounded source-over)
#   - gfx_fill_rect_blend / gfx_blit_blend (full-screen blend ops,
#     with bounds checks + dirty-rect tracking)
#   - per-window gui_window_fill_argb -> SYS_GUI_FILL_ARGB (=55)
#   - kernel-side display_test_alpha that asserts:
#       alpha=0   -> dst preserved
#       alpha=255 -> src.RGB overwrites
#       alpha=128 -> exact half-blend with rounding
#       alpha=0 idempotent
#   - userland 'rendertest alpha' that exercises 6 overlays + clipping
#     through the SYS_GUI_FILL_ARGB syscall path.
#
# Required signals (positive):
#   - "[PASS] display_alpha: src-over OK"  -- math is correct
#   - "[PASS] rendertest alpha"            -- syscall path works
#
# Forbidden tokens (negative):
#   - any "fill_rect_blend bounds violation" or "blit_blend bounds
#     violation"  -- means clip code lets a blend escape the surface
#   - kernel panics
#
# Re-runs the M27A baseline implicitly (we don't want a regression in
# the underlying display harness).


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m27c"
$serial   = Join-Path $LogDir "serial.$logTag.log"
$debug    = Join-Path $LogDir "debug.$logTag.log"
$qemuLog  = Join-Path $LogDir "qemu.$logTag.log"
$bootSentinel = '\[boot\] M27A: display .* PASS .* of \d+'
$timeoutSec   = 45

if (-not (Test-Path $iso))  { Write-Host "ISO not built" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M27C: Alpha Blending + Compositing ==============" -ForegroundColor Cyan

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
Write-Host "[m27c] qemu pid = $($proc.Id), waiting on $serial..."

$start = Get-Date
$saw   = $false
while (((Get-Date) - $start).TotalSeconds -lt $timeoutSec) {
    Start-Sleep -Seconds 1
    $log = Get-Content $serial -Raw -ErrorAction SilentlyContinue
    if (-not $log) { continue }
    if ($log -match $bootSentinel) { $saw = $true; break }
    if ($log -match 'panic|page fault|KERNEL OOPS') { break }
}
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

if (-not (Test-Path $serial)) {
    Write-Host "FAIL: $serial not produced" -ForegroundColor Red; exit 1
}
$txt = Get-Content $serial -Raw

if (-not $saw) {
    Write-Host "FAIL: timed out waiting for boot sentinel" -ForegroundColor Red
    Get-Content $serial -Tail 80; exit 1
}

$mustHave = @(
    # kernel-side math test (display_alpha) MUST be a PASS now (was
    # SKIP through M27B).
    '\[PASS\] display_alpha: src-over OK',
    # all four named results from the math assertion are emitted in
    # the same line; verifying the prefix is enough.
    # userland rendertest alpha must succeed.
    '\[PASS\] rendertest alpha\s+-- \d+ ARGB overlays committed',
    # M27A/B baseline still passes:
    '\[boot\] M27A: display \d+ PASS / 0 FAIL / 0 missing of \d+'
)
$missing = @()
foreach ($pat in $mustHave) {
    if ($txt -notmatch $pat) { $missing += $pat }
}

$forbidden = @(
    'panic',
    'page fault',
    'KERNEL OOPS',
    '\[FAIL\] ',
    '\[gfx\] fill_rect_blend bounds violation',
    '\[gfx\] blit_blend bounds violation',
    # display_alpha SKIP would mean we never wired the new test in.
    '\[SKIP\] display_alpha:'
)
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M27C PASS -- alpha math correct, syscall path works, no bounds escapes." -ForegroundColor Green
    Write-Host "Last 25 lines of $serial for visual sanity:"
    Get-Content $serial -Tail 25
    exit 0
} else {
    Write-Host "M27C FAIL" -ForegroundColor Red
    if ($missing.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($panics.Count -gt 0) {
        Write-Host "Forbidden tokens present:" -ForegroundColor Red
        $panics | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 100 lines of $serial ===" -ForegroundColor Yellow
    Get-Content $serial -Tail 100
    exit 1
}
