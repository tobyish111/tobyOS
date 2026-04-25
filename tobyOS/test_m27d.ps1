# test_m27d.ps1 -- M27D Improved Font Rendering.
#
# What M27D added:
#   - kernel-side gfx_draw_text_scaled / gfx_draw_text_smooth /
#     gfx_text_bounds (supersampled bitmap font + corner softening
#     instead of a TTF rasteriser)
#   - per-window gui_window_text_scaled  -> SYS_GUI_TEXT_SCALED (=56)
#   - kernel-side display_test_font that asserts the bounds calculator
#     for empty/single-line/multi-line strings at scales 1..3, plus
#     defensive clamp for negative/oversized scales
#   - userland /bin/fonttest with 6 cases: bitmap (legacy fallback),
#     scale2/scale4/smooth, mixed-character set, scale-clamp.
#
# Required signals (positive):
#   - "[PASS] display_font: scaled bounds OK"
#   - "[PASS] fonttest bitmap"     -- legacy 8x8 fallback intact
#   - "[PASS] fonttest scale2"
#   - "[PASS] fonttest scale4"
#   - "[PASS] fonttest smooth"
#   - "[PASS] fonttest mixed"
#   - "[PASS] fonttest clamp"
#   - "[fonttest] 6/6 PASS (0 FAIL)"
#   - M27A baseline still passes
#
# Forbidden tokens:
#   - kernel panics
#   - "fill_rect bounds violation" / "blit bounds violation" / blend
#     variants of same -- the smoothing pass uses gfx_fill_rect_blend
#     internally; if it ever escapes the surface the line would fire
#   - "[SKIP] display_font:"  -- means the new kernel test never wired

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m27d"
$serial   = "serial.$logTag.log"
$debug    = "debug.$logTag.log"
$qemuLog  = "qemu.$logTag.log"
$bootSentinel = '\[boot\] M27A: display .* PASS .* of \d+'
$timeoutSec   = 50

if (-not (Test-Path $iso))  { Write-Host "ISO not built" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M27D: Improved Font Rendering ==============" -ForegroundColor Cyan

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
Write-Host "[m27d] qemu pid = $($proc.Id), waiting on $serial..."

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
    '\[PASS\] display_font: scaled bounds OK',
    '\[PASS\] fonttest bitmap',
    '\[PASS\] fonttest scale2',
    '\[PASS\] fonttest scale4',
    '\[PASS\] fonttest smooth',
    '\[PASS\] fonttest mixed',
    '\[PASS\] fonttest clamp',
    '\[fonttest\] 6/6 PASS \(0 FAIL\)',
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
    '\[FAIL\] fonttest',
    '\[FAIL\] display_font',
    '\[gfx\] fill_rect bounds violation',
    '\[gfx\] blit bounds violation',
    '\[gfx\] fill_rect_blend bounds violation',
    '\[gfx\] blit_blend bounds violation',
    '\[SKIP\] display_font:'
)
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M27D PASS -- bounds calc correct, scaled+smooth path works, bitmap fallback intact." -ForegroundColor Green
    Write-Host "Last 30 lines of ${serial}:"
    Get-Content $serial -Tail 30
    exit 0
} else {
    Write-Host "M27D FAIL" -ForegroundColor Red
    if ($missing.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($panics.Count -gt 0) {
        Write-Host "Forbidden tokens present:" -ForegroundColor Red
        $panics | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 100 lines of ${serial} ===" -ForegroundColor Yellow
    Get-Content $serial -Tail 100
    exit 1
}
