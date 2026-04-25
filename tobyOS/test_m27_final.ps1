# test_m27_final.ps1 -- M27 final validation suite.
#
# Runs every per-phase validation (M27A..M27G) sequentially, then
# does a "global regression" sweep against the M27A boot log to
# confirm desktop / GUI input / shell / filesystem / networking are
# all alive and unchanged from prior milestones. Produces a single
# PASS/FAIL verdict and a comprehensive summary table.
#
# Usage:
#   pwsh .\test_m27_final.ps1            # run all phases + summary
#   pwsh .\test_m27_final.ps1 -Skip M27F # skip a phase by tag
#   pwsh .\test_m27_final.ps1 -Build     # `make` first to ensure ISO is fresh
#
# Exit 0 => everything PASS, exit 1 => at least one FAIL or regression.

param(
    [string[]] $Skip   = @(),
    [switch]   $Build  = $false
)

$ErrorActionPreference = 'Continue'

if ($Build) {
    Write-Host "[final] running 'make' to refresh tobyOS.iso ..." -ForegroundColor Cyan
    & make | Out-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[final] make failed -- aborting" -ForegroundColor Red
        exit 1
    }
}

if (-not (Test-Path "tobyOS.iso") -or -not (Test-Path "disk.img")) {
    Write-Host "[final] tobyOS.iso or disk.img missing -- run 'make' / 'make disk.img' first" -ForegroundColor Red
    exit 1
}

# --- per-phase configuration ----------------------------------------
$phases = @(
    [PSCustomObject]@{ Tag='M27A'; Script='.\test_m27a.ps1';
                       Desc='Display test harness (displayinfo / drawtest / rendertest / fonttest)' },
    [PSCustomObject]@{ Tag='M27B'; Script='.\test_m27b.ps1';
                       Desc='Rendering backend cleanup (gfx_backend abstraction + bounds checking)' },
    [PSCustomObject]@{ Tag='M27C'; Script='.\test_m27c.ps1';
                       Desc='Alpha blending + compositing (ARGB surfaces, source-over)' },
    [PSCustomObject]@{ Tag='M27D'; Script='.\test_m27d.ps1';
                       Desc='Improved font rendering (supersampling AA, scaled bitmap)' },
    [PSCustomObject]@{ Tag='M27E'; Script='.\test_m27e.ps1';
                       Desc='Dirty rectangles + redraw optimization (gfx_present_stats)' },
    [PSCustomObject]@{ Tag='M27F'; Script='.\test_m27f.ps1';
                       Desc='virtio-gpu support (probe/init/2D scanout/present_rect + Limine fallback)' },
    [PSCustomObject]@{ Tag='M27G'; Script='.\test_m27g.ps1';
                       Desc='Multi-monitor groundwork (origin_x/y, auto h-layout, register/unregister)' }
)

# --- run each phase --------------------------------------------------
$results = @()
foreach ($p in $phases) {
    if ($Skip -contains $p.Tag) {
        Write-Host ""
        Write-Host "============== Skipping $($p.Tag) (per -Skip) ==============" -ForegroundColor Yellow
        $results += [PSCustomObject]@{ Phase=$p.Tag; Verdict='SKIP'; Exit=$null; Desc=$p.Desc }
        continue
    }
    Write-Host ""
    Write-Host "============== Running $($p.Tag): $($p.Desc) ==============" -ForegroundColor Cyan
    $start = Get-Date
    & powershell -ExecutionPolicy Bypass -File $p.Script | Out-Host
    $rc = $LASTEXITCODE
    $elapsed = ((Get-Date) - $start).TotalSeconds
    $verdict = if ($rc -eq 0) { 'PASS' } else { 'FAIL' }
    Write-Host "[$($p.Tag)] $verdict (exit=$rc, ${elapsed}s)"
    $results += [PSCustomObject]@{ Phase=$p.Tag; Verdict=$verdict; Exit=$rc;
                                   Desc=$p.Desc; Elapsed=[Math]::Round($elapsed, 1) }
}

# --- global regression check ----------------------------------------
#
# Re-use the M27A serial log (smallest QEMU config -- network, USB
# kbd/mouse, default xHCI, NO virtio-gpu). This sweep proves that the
# display stack changes did NOT regress any prior-milestone
# subsystem. If the M27A run was skipped we degrade the sweep to
# SKIP rather than fail outright.
Write-Host ""
Write-Host "============== Global regression sweep (M27A serial log) ==============" -ForegroundColor Cyan
$globalChecks = @(
    # M27 spec deliverables (must hold across every milestone in this group)
    [PSCustomObject]@{ Subsystem='OS boots to desktop';     Pattern='\[gui\] window manager ready' },
    [PSCustomObject]@{ Subsystem='Login surfaced';          Pattern="window_create wid=\d+ owner_pid=\d+ size=\d+x\d+ pos=\(\d+,\d+\) title='tobyOS login'" },
    [PSCustomObject]@{ Subsystem='displayinfo userland';    Pattern='PASS: displayinfo: \d+ output\(s\); primary fb0 \d+x\d+' },
    [PSCustomObject]@{ Subsystem='drawtest userland';       Pattern='PASS: drawtest: \d+ primitive\(s\) ran cleanly' },
    [PSCustomObject]@{ Subsystem='rendertest userland';     Pattern='PASS: rendertest: pass=\d+ skip=\d+ fail=0' },
    [PSCustomObject]@{ Subsystem='fonttest userland';       Pattern='\[boot\] M27A: /bin/fonttest .*PASS' },
    [PSCustomObject]@{ Subsystem='gfx_backend abstraction'; Pattern='\[gfx\] back buffer \d+x\d+ \(\d+ KiB\) ready, fb_pitch=\d+ px, backend=(limine-fb|virtio-gpu)' },
    [PSCustomObject]@{ Subsystem='Alpha blending';          Pattern='\[PASS\] display_alpha:' },
    [PSCustomObject]@{ Subsystem='Font rendering (M27D)';   Pattern='\[PASS\] display_font:' },
    [PSCustomObject]@{ Subsystem='Dirty redraw stats';      Pattern='\[PASS\] display_dirty: tracker OK.*\(stats: \d+ flips' },
    [PSCustomObject]@{ Subsystem='Multi-monitor (M27G)';    Pattern='\[PASS\] display_multi: multi OK: 3 outputs registered' },
    [PSCustomObject]@{ Subsystem='Display layout summary';  Pattern='\[display\] \d+ output\(s\) registered .* layout=\d+x\d+@\(\-?\d+,\-?\d+\)' },
    # Cross-milestone regressions we must not introduce
    [PSCustomObject]@{ Subsystem='Filesystem';              Pattern="\[initrd\] using '/boot/initrd.tar'" },
    [PSCustomObject]@{ Subsystem='Shell';                   Pattern='\[boot\] M26A: driving shell builtins' },
    [PSCustomObject]@{ Subsystem='GUI ready';               Pattern='\[gui\] window manager ready' },
    [PSCustomObject]@{ Subsystem='Input (PS/2)';            Pattern='\[PASS\] input: ps2 kbd' },
    [PSCustomObject]@{ Subsystem='Input (USB)';             Pattern='usb_hid devs=\d+ \(kbd=\d+ mouse=\d+\)' },
    [PSCustomObject]@{ Subsystem='Network';                 Pattern='\[net\] DHCP lease applied' },
    [PSCustomObject]@{ Subsystem='ACPI';                    Pattern='\[acpi\] AML inventory: dsdt=\d+ bytes' },
    [PSCustomObject]@{ Subsystem='Mouse cursor';            Pattern='heartbeat windows=\d+ apps_alive=\d+ cursor=\(\d+,\d+\)' }
)

$globalResults = @()
$serialA = "serial.m27a.log"
if (Test-Path $serialA) {
    $logA = Get-Content $serialA -Raw
    foreach ($g in $globalChecks) {
        $hit = $logA -match $g.Pattern
        $globalResults += [PSCustomObject]@{
            Subsystem = $g.Subsystem
            Verdict   = if ($hit) { 'PASS' } else { 'FAIL' }
            Pattern   = $g.Pattern
        }
    }
} else {
    Write-Host "[final] WARN: $serialA missing (M27A skipped or failed) -- global sweep skipped" -ForegroundColor Yellow
    foreach ($g in $globalChecks) {
        $globalResults += [PSCustomObject]@{ Subsystem=$g.Subsystem; Verdict='SKIP'; Pattern=$g.Pattern }
    }
}

# --- forbidden-tokens sweep ----------------------------------------
#
# Across EVERY serial.m27*.log we just produced, none of these
# tokens may appear. They represent regressions the M27 spec calls
# out as forbidden (no panic, no bounds violation, no [FAIL]).
Write-Host ""
Write-Host "============== Cross-log forbidden-token sweep ==============" -ForegroundColor Cyan
$forbiddenTokens = @(
    'panic',
    'page fault',
    'KERNEL OOPS',
    '\[gfx\] fill_rect bounds violation',
    '\[gfx\] blit bounds violation',
    '\[gfx\] fill_rect_blend bounds violation',
    '\[gfx\] blit_blend bounds violation',
    '\[FAIL\] display_render',
    '\[FAIL\] display_alpha',
    '\[FAIL\] display_font',
    '\[FAIL\] display_dirty',
    '\[FAIL\] display_multi',
    '\[virtio-gpu\] flush: TRANSFER failed',
    '\[virtio-gpu\] flush: FLUSH failed',
    '\[virtio-gpu\] present_rect: TRANSFER failed',
    '\[virtio-gpu\] present_rect: FLUSH failed'
)

$forbiddenHits = @()
$logs = Get-ChildItem -Path . -Filter 'serial.m27*.log' -ErrorAction SilentlyContinue
foreach ($lf in $logs) {
    $body = Get-Content $lf.FullName -Raw
    foreach ($tok in $forbiddenTokens) {
        if ($body -match $tok) {
            $forbiddenHits += [PSCustomObject]@{ Log=$lf.Name; Token=$tok }
        }
    }
}
if ($forbiddenHits.Count -eq 0) {
    Write-Host "  no forbidden tokens detected across $($logs.Count) M27 log file(s)" -ForegroundColor Green
} else {
    Write-Host "  found $($forbiddenHits.Count) forbidden-token hit(s):" -ForegroundColor Red
    $forbiddenHits | Format-Table -AutoSize | Out-String | Write-Host
}

# --- summary --------------------------------------------------------
Write-Host ""
Write-Host "================== M27 PER-PHASE RESULTS ==================" -ForegroundColor Cyan
$results | Select-Object Phase, Verdict, Elapsed, Desc | Format-Table -AutoSize -Wrap | Out-String | Write-Host

Write-Host "================== GLOBAL REGRESSION SWEEP ==================" -ForegroundColor Cyan
$globalResults | Select-Object Subsystem, Verdict | Format-Table -AutoSize | Out-String | Write-Host

# --- known limitations ----------------------------------------------
Write-Host "================== KNOWN LIMITATIONS ==================" -ForegroundColor Cyan
@(
    "M27A: drawtest/rendertest run with the GUI active; the compositor owns the back buffer, so on-screen visual diff is left to a human / screenshot.",
    "M27B: gfx_backend exposes flip/present_rect/describe only -- no surface/buffer creation API, the back buffer is global per-display.",
    "M27C: alpha blending is per-pixel source-over; no premultiplied-alpha optimization, no Porter-Duff modes beyond OVER, no SIMD path.",
    "M27D: no TrueType -- supersampled bitmap font + 'corner-smoothing' AA pass. Visually clean for ASCII but not Unicode-aware.",
    "M27E: dirty union is a single bounding box (not a region list); >=95% coverage falls back to a full present.",
    "M27F: virtio-gpu uses synchronous TRANSFER+FLUSH (no virtqueue queueing); IRQ is wired but only used as a completion observable. No 3D / virgl.",
    "M27F: only scanout 0 is driven. Secondary scanouts the host advertises are reported via describe() but cannot be targeted.",
    "M27G: multi-monitor support is groundwork only. Auto horizontal layout works, register/unregister/set_origin work, ABI carries origin_x/origin_y, but the desktop layer still composites only into the primary scanout.",
    "M27G: synthetic-multi devtest uses 'vmon1'/'vmon2' registry slots with no backing surface; real second-monitor present_rect would need a per-output gfx_backend."
) | ForEach-Object { Write-Host "  - $_" }

# --- QEMU validation commands ---------------------------------------
Write-Host ""
Write-Host "================== QEMU VALIDATION COMMANDS ==================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Per-phase scripts (each is independent; reproduces a single QEMU boot):"
Write-Host "    pwsh .\test_m27a.ps1     # display test harness"
Write-Host "    pwsh .\test_m27b.ps1     # rendering backend cleanup"
Write-Host "    pwsh .\test_m27c.ps1     # alpha blending"
Write-Host "    pwsh .\test_m27d.ps1     # font rendering"
Write-Host "    pwsh .\test_m27e.ps1     # dirty rectangles"
Write-Host "    pwsh .\test_m27f.ps1     # virtio-gpu support (TWO boots: with + without -device virtio-gpu-pci)"
Write-Host "    pwsh .\test_m27g.ps1     # multi-monitor groundwork"
Write-Host ""
Write-Host "  Or run this aggregator:"
Write-Host "    pwsh .\test_m27_final.ps1            # all phases + global sweep"
Write-Host "    pwsh .\test_m27_final.ps1 -Build     # rebuild kernel ISO first"
Write-Host "    pwsh .\test_m27_final.ps1 -Skip M27F # skip a phase by tag"
Write-Host ""
Write-Host "  Direct QEMU invocations -- framebuffer fallback (Limine-fb backend):"
Write-Host '    qemu-system-x86_64 -cdrom tobyOS.iso \'
Write-Host '        -drive file=disk.img,format=raw,if=ide,index=0,media=disk -smp 4 \'
Write-Host '        -device qemu-xhci,id=usb0 -device usb-kbd,bus=usb0.0 \'
Write-Host '        -device usb-mouse,bus=usb0.0 -netdev user,id=net0 \'
Write-Host '        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \'
Write-Host '        -serial file:serial.log -no-reboot'
Write-Host ""
Write-Host "  Direct QEMU invocations -- virtio-gpu backend (M27F):"
Write-Host '    qemu-system-x86_64 -cdrom tobyOS.iso \'
Write-Host '        -drive file=disk.img,format=raw,if=ide,index=0,media=disk -smp 4 \'
Write-Host '        -device qemu-xhci,id=usb0 -device usb-kbd,bus=usb0.0 \'
Write-Host '        -device usb-mouse,bus=usb0.0 -netdev user,id=net0 \'
Write-Host '        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \'
Write-Host '        -vga none -device virtio-gpu-pci \'
Write-Host '        -serial file:serial.log -no-reboot'

# --- real-hardware test checklist ----------------------------------
Write-Host ""
Write-Host "================== REAL-HARDWARE DISPLAY VALIDATION CHECKLIST ==================" -ForegroundColor Cyan
@(
    "[ ] Boot the ISO on a bare-metal x86_64 PC. Limine should pick a sane native VBE FB.",
    "[ ] Confirm the desktop renders at native resolution. The login window must be readable.",
    "[ ] Run 'displayinfo'. Expect: 1 output, backend=limine-fb, layout=WxH matching the panel.",
    "[ ] Run 'displayinfo --json'. Confirm origin_x:0, origin_y:0, plus a trailing {`"layout`":...} record.",
    "[ ] Run 'drawtest'. PASS expected. The GUI window opens; close it manually.",
    "[ ] Run 'rendertest'. All 8 cases PASS or SKIP cleanly (alpha/font/dirty may SKIP if compositor is idle).",
    "[ ] Run 'fonttest'. Multi-line scaled-font window must render without artifacts.",
    "[ ] Drag a window across the screen. No tearing / smearing / dirty trails (M27E sanity).",
    "[ ] Open two windows and drag one over the other. Both repaint cleanly on overlap (M27E + M27C).",
    "[ ] Move the cursor over a translucent UI element. The cursor stays on top, blending visible (M27C).",
    "[ ] Disconnect / reconnect HDMI mid-session if the firmware tolerates it. We DO NOT support live re-init; the system should stay alive (no panic). Reboot to recover.",
    "[ ] On a system with virtio-gpu (e.g. another VM): boot + run 'displayinfo'. Backend MUST be virtio-gpu, with pci=BB:SS.F + scanouts=N/16 in the JSON describe field.",
    "[ ] On a multi-head firmware (rare on bare metal): the secondary scanout will be reported via [virtio-gpu] scanout N: ... lines but only scanout 0 is composited into."
) | ForEach-Object { Write-Host "  $_" }

# --- final verdict --------------------------------------------------
Write-Host ""
$failedPhases = @($results       | Where-Object { $_.Verdict -eq 'FAIL' }).Count
$failedGlobal = @($globalResults | Where-Object { $_.Verdict -eq 'FAIL' }).Count
$failedToken  = $forbiddenHits.Count
$totalFail    = $failedPhases + $failedGlobal + $failedToken

if ($totalFail -eq 0) {
    Write-Host "================== M27 FINAL: PASS ==================" -ForegroundColor Green
    Write-Host "All M27 phases passed, no global regression detected, no forbidden tokens in any M27 log." -ForegroundColor Green
    exit 0
} else {
    Write-Host "================== M27 FINAL: FAIL ==================" -ForegroundColor Red
    Write-Host "$failedPhases per-phase failure(s) + $failedGlobal global regression(s) + $failedToken forbidden-token hit(s)." -ForegroundColor Red
    exit 1
}
