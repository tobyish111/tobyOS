# test_m27e.ps1 -- M27E Dirty Rectangles / Redraw Optimisation.
#
# What M27E added:
#   - gfx_present_stats counters (total/full/partial/empty + pixels)
#   - gfx_dirty_clear() so the compositor can REPLACE the per-primitive
#     dirty union with a smaller compositor-level hint just before flip
#   - gui_invalidate_rect / gui_invalidate_full / gui_invalidate_stats
#   - compositor_pass now consumes the hint union and falls back to a
#     full present whenever inv_full is set or the hint covers >=95%
#     of the screen
#   - mouse-cursor moves hint just the 12x19 sprite bbox (old + new)
#   - window flips hint just the window's outer rect
#   - drag in progress hints both old + new outer rects
#   - z-raise / window create / window close force a full present
#   - new SYS_DISPLAY_PRESENT_STATS (=57) for userland snapshotting
#   - userland 'rendertest dirty' takes a stats snapshot, opens a
#     window, flips it 4 times, snapshots again, asserts the deltas
#     are sane and emits the pixel deltas for visual confirmation
#
# Required signals:
#   - "[PASS] display_dirty: tracker OK ... (stats: N flips, ...)"
#     -- proves the kernel-side accumulator + the new stats counter
#     are wired together
#   - "[PASS] rendertest dirty"  -- userland round-trip works
#   - the rendertest dirty PASS message contains "compositor frames +N"
#     where N >= 1, proving the compositor's per-frame counter moved
#   - M27A baseline still passes
#
# Forbidden tokens:
#   - kernel panic / bounds violation
#   - "[FAIL] rendertest dirty"
#   - the literal "[SKIP] rendertest dirty" -- a SKIP at boot would
#     mean compositor never flipped, which would itself be a
#     regression (prior milestones had it flipping once at boot)

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m27e"
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
Write-Host "============== M27E: Dirty Rectangles ==============" -ForegroundColor Cyan

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
Write-Host "[m27e] qemu pid = $($proc.Id), waiting on ${serial}..."

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
    '\[PASS\] display_dirty: tracker OK.*\(stats: \d+ flips, \d+ full, \d+ partial, \d+ empty\)',
    '\[PASS\] rendertest dirty\s+-- flips \+\d+ \(full\+\d+ partial\+\d+\); compositor frames \+\d+',
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
    '\[FAIL\] rendertest dirty',
    '\[FAIL\] display_dirty',
    '\[gfx\] fill_rect bounds violation',
    '\[gfx\] blit bounds violation',
    '\[gfx\] fill_rect_blend bounds violation',
    '\[gfx\] blit_blend bounds violation'
)
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M27E PASS -- dirty-rect tracker live, compositor consumes hints, stats wired." -ForegroundColor Green
    Write-Host "Last 30 lines of ${serial}:"
    Get-Content $serial -Tail 30
    exit 0
} else {
    Write-Host "M27E FAIL" -ForegroundColor Red
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
