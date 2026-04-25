# test_m27f.ps1 -- M27F virtio-gpu Support validation.
#
# What M27F validates / added:
#   - virtio-gpu PCI driver (vid 1AF4, did 1050) probes a guest
#     virtio-gpu-pci device and runs the four-step setup
#       (GET_DISPLAY_INFO / RESOURCE_CREATE_2D /
#        RESOURCE_ATTACH_BACKING / SET_SCANOUT)
#     cleanly at boot
#   - the gfx_backend abstraction picks up the virtio-gpu backend so
#     gfx_flip() routes through TRANSFER_TO_HOST_2D + RESOURCE_FLUSH
#   - present_rect is now wired (M27F new): the dirty-rect path also
#     flows through partial TRANSFER+FLUSH commands
#   - displayinfo correctly identifies the active backend
#     (backend=virtio-gpu, backend_id=2)
#   - the kernel display_render selftest reports present_rect=yes,
#     describe=yes and surfaces the device's PCI BDF in `extra`
#   - the kernel display_dirty selftest passes -- proving the partial
#     TRANSFER+FLUSH path actually fires
#   - all userland rendertest cases pass (basic, geometry, primitives,
#     overlap, alpha, font, dirty, backend) just like M27A baseline
#   - drawtest passes
#   - no kernel panic / page fault / bounds violation
#   - the universal Limine-fb fallback is unchanged: a second boot
#     WITHOUT -device virtio-gpu-pci still produces backend=limine-fb
#     and all the same PASS lines (regression guard for M27B/E)
#
# QEMU configuration:
#   - first boot:  -vga none -device virtio-gpu-pci
#       The QEMU vgabios-virtio.bin option ROM gives Limine no VBE FB
#       (since `-vga none` removes the std VGA), so gfx_layer_init
#       returns empty and virtio_gpu_install_backend() takes the
#       "gfx not ready -- bring up gfx against GPU backing" branch.
#       This guarantees the active backend is the virtio-gpu one.
#
#   - second boot: SAME args without -device virtio-gpu-pci, default
#       std VGA. Limine gets a stdvga FB, gfx_layer_init succeeds,
#       virtio_gpu_install_backend() is a silent no-op (g_vgpu_bound
#       is false), and the active backend stays limine-fb. This
#       exercises the "fallback works" requirement from the M27 spec.

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$bootSentinel = '\[boot\] M27A: display .* PASS .* of \d+'
$timeoutSec   = 50

if (-not (Test-Path $iso))  {
    Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $disk)) {
    Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red
    exit 1
}

function Stop-QemuAndWait {
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}

function Run-Boot {
    param(
        [string]   $tag,
        [string[]] $extraArgs
    )
    $serial  = "serial.$tag.log"
    $debug   = "debug.$tag.log"
    $qemuLog = "qemu.$tag.log"

    Stop-QemuAndWait
    Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

    $args = @(
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
    $args = $args + $extraArgs

    $proc = Start-Process -FilePath $qemu -ArgumentList $args -PassThru
    Write-Host "[$tag] qemu pid = $($proc.Id), waiting on $serial (timeout ${timeoutSec}s)..."

    $start = Get-Date
    $saw   = $false
    while (((Get-Date) - $start).TotalSeconds -lt $timeoutSec) {
        Start-Sleep -Seconds 1
        if (-not (Test-Path $serial)) { continue }
        $log = Get-Content $serial -Raw -ErrorAction SilentlyContinue
        if (-not $log) { continue }
        if ($log -match $bootSentinel)              { $saw = $true; break }
        if ($log -match 'panic|page fault|KERNEL OOPS') { break }
    }
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400

    if (-not (Test-Path $serial)) {
        Write-Host "FAIL [$tag]: $serial not produced by QEMU" -ForegroundColor Red
        return $null
    }
    if (-not $saw) {
        Write-Host "FAIL [$tag]: timed out waiting for boot sentinel" -ForegroundColor Red
        Get-Content $serial -Tail 80
        return $null
    }
    return Get-Content $serial -Raw
}

function Check-Patterns {
    param(
        [string]   $tag,
        [string]   $txt,
        [string[]] $mustHave,
        [string[]] $forbidden
    )
    $missing = @()
    foreach ($pat in $mustHave) {
        if ($txt -notmatch $pat) { $missing += $pat }
    }
    $bad = @()
    foreach ($pat in $forbidden) {
        if ($txt -match $pat) { $bad += $pat }
    }
    if ($missing.Count -gt 0) {
        Write-Host "[$tag] missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($bad.Count -gt 0) {
        Write-Host "[$tag] forbidden tokens present:" -ForegroundColor Red
        $bad | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    return ($missing.Count -eq 0 -and $bad.Count -eq 0)
}

# ---------------------------------------------------------------
# Pass 1: -device virtio-gpu-pci  (virtio-gpu IS the backend)
# ---------------------------------------------------------------
Write-Host ""
Write-Host "============== M27F: virtio-gpu (pass 1: virtio-gpu active) ==============" -ForegroundColor Cyan

$txt1 = Run-Boot -tag "m27f_vgpu" -extraArgs @(
    "-vga", "none",
    "-device", "virtio-gpu-pci"
)
if ($null -eq $txt1) { exit 1 }

$mustHave1 = @(
    # Driver scaffolding came up.
    '\[virtio-gpu\] probing [0-9a-f]+:[0-9a-f]+\.[0-9a-f]+\s+\(vid:did 1af4:1050\)',
    '\[virtio-gpu\] features:.*driver=',
    '\[virtio-gpu\] scanout 0:\s+\d+x\d+\s+\(preferred\)',
    '\[virtio-gpu\] RESOURCE_CREATE_2D ok',
    '\[virtio-gpu\] RESOURCE_ATTACH_BACKING ok',
    '\[virtio-gpu\] SET_SCANOUT 0 -> resource \d+ ok',
    # The PCI bind line confirms the registry hooked the driver.
    '\[pci\] driver virtio-gpu bound to ',
    # Backend got installed (the M27F-critical signal).
    '\[virtio-gpu\] backend installed -- gfx_flip now uses TRANSFER\+FLUSH on scanout 0',
    '\[gfx\] backend = virtio-gpu',
    # Display registry picked it up.
    '\[display\] registered fb0 \d+x\d+ backend=virtio-gpu',
    # Kernel-side display selftests now report virtio-gpu as the backend
    # AND prove present_rect/describe are populated.
    '\[PASS\] display: 1 output\(s\); primary fb0 \d+x\d+ pitch=\d+ bpp=32 backend=virtio-gpu',
    '\[PASS\] display_render:.*backend=virtio-gpu',
    '\[PASS\] display_render:.*present_rect=yes',
    '\[PASS\] display_render:.*describe=yes',
    '\[PASS\] display_render:.*pci=[0-9a-f]+:[0-9a-f]+\.[0-9a-f]+',
    # The dirty-rect path actually exercises virtio-gpu present_rect
    # now (the M27F-critical signal). Without M27F''s vgpu_present_rect
    # the kernel selftest would FAIL with "gfx_flip never took partial-
    # present path".
    '\[PASS\] display_dirty: tracker OK.*\(stats: \d+ flips, \d+ full, [1-9]\d* partial, \d+ empty\)',
    '\[PASS\] display_alpha:',
    '\[PASS\] display_font:',
    # Boot harness userland tools.
    '\[boot\] M27A: /bin/displayinfo .*PASS\)',
    '\[boot\] M27A: /bin/drawtest .*PASS\)',
    '\[boot\] M27A: /bin/rendertest .*PASS\)',
    '\[boot\] M27A: /bin/fonttest .*PASS\)',
    'PASS: drawtest: \d+ primitive\(s\) ran cleanly',
    'PASS: displayinfo: \d+ output\(s\); primary fb0 \d+x\d+ backend=virtio-gpu',
    # All eight rendertest cases must pass / skip cleanly.
    '\[PASS\] rendertest basic\s+-- fb0 \d+x\d+ backend=virtio-gpu',
    '\[PASS\] rendertest geometry',
    '\[PASS\] rendertest primitives',
    '\[PASS\] rendertest overlap',
    '\[(PASS|SKIP)\] rendertest cursor',
    '\[PASS\] rendertest backend\s+-- backend=virtio-gpu id=2 \(mapped=virtio-gpu\)',
    '\[(PASS|SKIP)\] rendertest alpha',
    '\[(PASS|SKIP)\] rendertest font',
    '\[(PASS|SKIP)\] rendertest dirty',
    'PASS: rendertest: pass=\d+ skip=\d+ fail=0',
    # Final boot harness summary.
    '\[boot\] M27A: display 5 PASS / 0 FAIL / 0 missing of 5',
    # JSON output identifies backend_id=2 (ABI_DISPLAY_BACKEND_VIRTIO_GPU).
    '"backend":"virtio-gpu","backend_id":2'
)
$forbidden1 = @(
    'panic',
    'page fault',
    'KERNEL OOPS',
    '\[FAIL\] ',
    '\[gfx\] fill_rect bounds violation',
    '\[gfx\] blit bounds violation',
    '\[gfx\] fill_rect_blend bounds violation',
    '\[gfx\] blit_blend bounds violation',
    # If virtio-gpu's TRANSFER or FLUSH ever fails, the backend
    # auto-deactivates with these log lines. Their presence on a clean
    # QEMU boot would be an outright regression.
    '\[virtio-gpu\] flush: TRANSFER failed',
    '\[virtio-gpu\] flush: FLUSH failed',
    '\[virtio-gpu\] present_rect: TRANSFER failed',
    '\[virtio-gpu\] present_rect: FLUSH failed'
)
$ok1 = Check-Patterns -tag "m27f_vgpu" -txt $txt1 `
                      -mustHave $mustHave1 -forbidden $forbidden1

# ---------------------------------------------------------------
# Pass 2: NO virtio-gpu  (regression guard: limine-fb fallback)
# ---------------------------------------------------------------
Write-Host ""
Write-Host "============== M27F: fallback (pass 2: no virtio-gpu) ==============" -ForegroundColor Cyan

$txt2 = Run-Boot -tag "m27f_fb" -extraArgs @()
if ($null -eq $txt2) { exit 1 }

$mustHave2 = @(
    # No virtio-gpu device in the guest -> probe never runs.
    # Backend stays limine-fb.
    '\[gfx\] back buffer .* backend=limine-fb',
    '\[display\] registered fb0 \d+x\d+ backend=limine-fb',
    '\[PASS\] display:.*backend=limine-fb',
    '\[PASS\] display_render:.*backend=limine-fb',
    '\[PASS\] display_render:.*present_rect=yes',
    '\[PASS\] display_dirty: tracker OK',
    'PASS: displayinfo: \d+ output\(s\); primary fb0 \d+x\d+ backend=limine-fb',
    '\[PASS\] rendertest backend\s+-- backend=limine-fb id=1 \(mapped=limine-fb\)',
    '\[boot\] M27A: display 5 PASS / 0 FAIL / 0 missing of 5'
)
$forbidden2 = @(
    'panic',
    'page fault',
    'KERNEL OOPS',
    '\[FAIL\] ',
    # The virtio-gpu probe should NEVER run when the device isn''t
    # present. If we see "probing" here something injected a phantom
    # device, which is a regression in the PCI matcher.
    '\[virtio-gpu\] probing'
)
$ok2 = Check-Patterns -tag "m27f_fb" -txt $txt2 `
                      -mustHave $mustHave2 -forbidden $forbidden2

# ---------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------
Write-Host ""
if ($ok1 -and $ok2) {
    Write-Host "M27F PASS -- virtio-gpu backend installs + drives full M27A harness; limine-fb fallback unaffected." -ForegroundColor Green
    Write-Host ""
    Write-Host "Last 20 lines of serial.m27f_vgpu.log:"
    Get-Content "serial.m27f_vgpu.log" -Tail 20
    Write-Host ""
    Write-Host "Last 8 lines of serial.m27f_fb.log:"
    Get-Content "serial.m27f_fb.log" -Tail 8
    exit 0
} else {
    Write-Host "M27F FAIL" -ForegroundColor Red
    if (-not $ok1) {
        Write-Host ""
        Write-Host "=== last 80 lines of serial.m27f_vgpu.log ===" -ForegroundColor Yellow
        Get-Content "serial.m27f_vgpu.log" -Tail 80
    }
    if (-not $ok2) {
        Write-Host ""
        Write-Host "=== last 80 lines of serial.m27f_fb.log ===" -ForegroundColor Yellow
        Get-Content "serial.m27f_fb.log" -Tail 80
    }
    exit 1
}
