# test_m27g.ps1 -- M27G Multi-Monitor Groundwork validation.
#
# What M27G added (groundwork only -- no second physical scanout yet):
#   - struct abi_display_info gained int32_t origin_x/origin_y plus a
#     u32 _pad0 (96 bytes total). _Static_assert in abi.h validates
#     the new layout at compile time.
#   - kernel display.c got display_unregister_output(),
#     display_set_origin(), display_total_bounds(), and the new
#     display_test_multi() devtest. Auto-horizontal layout: each new
#     output is appended to the right of the rightmost existing one.
#     Re-registering an existing output preserves its origin so a
#     backend swap doesn't reshuffle the monitor layout.
#   - libtoby tobydisp_print_header / _print_record gained an ORIGIN
#     column.
#   - userland displayinfo prints layout=WxH on the PASS summary line
#     and emits an extra {"layout":...} JSON record with the bounding
#     box.
#   - virtio-gpu enumerates every host-advertised scanout, caches the
#     pmodes[], reports enabled_scanouts via describe(), and logs
#     informational lines for any secondary scanout the host reports.
#   - display_dump_kprintf includes layout=WxH@(x,y) on its summary
#     line and prints @(origin_x,origin_y) per output.
#
# Validation strategy (single QEMU pass):
#   1. Boot the standard QEMU config -- single monitor (limine-fb).
#      Single-monitor system MUST still work (M27 spec requirement).
#   2. The kernel-side display_multi devtest runs at boot; it
#      synthetically registers two extra outputs ("vmon1" 1024x768 and
#      "vmon2" 800x600), validates the auto-horizontal layout math,
#      exercises display_set_origin(), then unregisters both. The
#      registry MUST be back to its pre-test state at exit. This
#      simulates a multi-monitor scenario without needing a second
#      physical scanout.
#   3. After the synthetic test runs, the registry has exactly the
#      pre-existing real outputs again, and userland displayinfo
#      output is unchanged.
#
# Required signals:
#   - "[PASS] display_multi: multi OK: 3 outputs registered..."
#   - "layout=WxH" on displayinfo summary line (single-monitor still
#     works; layout == primary geometry)
#   - "[boot] M27A: display N PASS / 0 FAIL / 0 missing" still passes
#     -- no userland regression
#   - the per-output [INFO] display: ... line includes "@(0,0)"
#     (or some signed origin), proving display_dump_kprintf was
#     updated
#   - the [display] N output(s) registered ... layout=WxH@(x,y)
#     summary header is produced
#
# Forbidden tokens:
#   - kernel panic / page fault / OOPS
#   - "[FAIL] display_multi" -- the M27G groundwork would be broken
#   - "registry leak" or "primary changed" anywhere in the multi
#     test message (substrings only the failure paths emit)


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m27g"
$serial   = Join-Path $LogDir "serial.$logTag.log"
$debug    = Join-Path $LogDir "debug.$logTag.log"
$qemuLog  = Join-Path $LogDir "qemu.$logTag.log"
$bootSentinel = '\[boot\] M27A: display .* PASS .* of \d+'
$timeoutSec   = 50

if (-not (Test-Path $iso))  { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M27G: Multi-Monitor Groundwork ==============" -ForegroundColor Cyan

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
Write-Host "[m27g] qemu pid = $($proc.Id), waiting on ${serial}..."

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

# M27G must-haves. Order matters only insofar as we want clear failure
# messages -- each pattern is checked independently.
$mustHave = @(
    # Synthetic multi-monitor exercise must PASS. The trailing detail
    # is what the M27G display_test_multi function emits on success.
    '\[PASS\] display_multi: multi OK: 3 outputs registered',
    # display_dump_kprintf summary header now carries layout=WxH@(x,y).
    '\[display\] \d+ output\(s\) registered .* layout=\d+x\d+@\(\-?\d+,\-?\d+\)',
    # Per-output line includes signed origin (single-monitor system
    # always sits at (0,0) but we accept any signed pair).
    '\[INFO\] display: \w+.*\d+x\d+@\(\-?\d+,\-?\d+\)',
    # Userland displayinfo summary now ends with layout=WxH. The
    # single-monitor case has layout == primary geometry, so layout
    # MUST be non-zero.
    'PASS: displayinfo: \d+ output\(s\); primary fb0 \d+x\d+ backend=\S+ layout=[1-9]\d*x[1-9]\d*',
    # M27A baseline still healthy (no regression).
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
    '\[FAIL\] display_multi',
    '\[FAIL\] display:',
    '\[FAIL\] display_render:',
    # These substrings only appear on display_test_multi failure paths.
    'registry leak',
    'primary changed',
    'set_origin didn',
    # Bounds violations from the gfx layer would be M27B regression.
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
    Write-Host "M27G PASS -- multi-monitor groundwork live; auto-horizontal layout, register/unregister, set_origin, total_bounds, ABI extension all healthy." -ForegroundColor Green
    Write-Host ""
    Write-Host "Relevant excerpts:"
    $excerpts = $txt -split "`n" | Where-Object {
        $_ -match 'display_multi|layout=|origin' -and
        $_ -notmatch '^\s*$'
    } | Select-Object -First 12
    $excerpts | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
    Write-Host "Last 20 lines of ${serial}:"
    Get-Content $serial -Tail 20
    exit 0
} else {
    Write-Host "M27G FAIL" -ForegroundColor Red
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
