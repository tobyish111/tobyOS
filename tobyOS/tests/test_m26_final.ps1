# test_m26_final.ps1 -- M26 final validation suite.
#
# Runs every per-phase validation (M26A..M26G) sequentially, then
# does a "global regression" sweep against the M26A boot log to
# confirm shell / GUI input / filesystem / networking are all alive
# and unchanged from prior milestones. Produces a single PASS/FAIL
# verdict and a comprehensive summary table.
#
# Usage (from tobyOS repo root, or any cwd after the script sets location):
#   pwsh tests/test_m26_final.ps1            # run all phases + summary
#   pwsh tests/test_m26_final.ps1 -Skip M26B # skip a phase by tag
#   pwsh tests/test_m26_final.ps1 -Build     # `make` first to ensure ISO is fresh
#
# Exit 0 => everything PASS, exit 1 => at least one FAIL or regression.

param(
    [string[]] $Skip   = @(),
    [switch]   $Build  = $false
)


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

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
    [PSCustomObject]@{ Tag='M26A'; Script=(Join-Path $PSScriptRoot 'test_m26a.ps1');
                       Desc='Peripheral test harness + userland tools' },
    [PSCustomObject]@{ Tag='M26B'; Script=(Join-Path $PSScriptRoot 'test_m26b.ps1');
                       Desc='USB hub support (descriptors, port walk, devlist)' },
    [PSCustomObject]@{ Tag='M26C'; Script=(Join-Path $PSScriptRoot 'test_m26c.ps1');
                       Desc='USB hot-plug (attach/detach, hot_event ring)' },
    [PSCustomObject]@{ Tag='M26D'; Script=(Join-Path $PSScriptRoot 'test_m26d.ps1');
                       Desc='USB HID robustness (multi-dev, modifiers, reconnect, PS/2 fallback)' },
    [PSCustomObject]@{ Tag='M26E'; Script=(Join-Path $PSScriptRoot 'test_m26e.ps1');
                       Desc='USB mass storage robustness (mount/unmount, safe removal)' },
    [PSCustomObject]@{ Tag='M26F'; Script=(Join-Path $PSScriptRoot 'test_m26f.ps1');
                       Desc='HD Audio basic output (CRST, CORB/RIRB, codec walk, tone)' },
    [PSCustomObject]@{ Tag='M26G'; Script=(Join-Path $PSScriptRoot 'test_m26g.ps1');
                       Desc='ACPI battery (heuristic AML scan + fw_cfg mock)' }
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
# Re-use the M26A serial log (smallest QEMU config -- network, USB
# kbd/mouse, default xHCI). If shell / GUI / FS / network signals
# are missing there, something earlier broke and only one of the
# per-phase scripts would have caught it as a side effect. This is
# the explicit guard rail.
Write-Host ""
Write-Host "============== Global regression sweep (M26A serial log) ==============" -ForegroundColor Cyan
$globalChecks = @(
    [PSCustomObject]@{ Subsystem='OS boot';      Pattern='\[boot\] M26A: userland (\d+) PASS / 0 FAIL / 0 missing of \1' },
    [PSCustomObject]@{ Subsystem='Filesystem';   Pattern="\[initrd\] using '/boot/initrd.tar'" },
    [PSCustomObject]@{ Subsystem='Shell';        Pattern='\[boot\] M26A: driving shell builtins' },
    [PSCustomObject]@{ Subsystem='GUI';          Pattern='\[gui\] window manager ready' },
    [PSCustomObject]@{ Subsystem='Input (PS/2)'; Pattern='\[PASS\] input: ps2 kbd' },
    [PSCustomObject]@{ Subsystem='Input (USB)';  Pattern='usb_hid devs=\d+ \(kbd=\d+ mouse=\d+\)' },
    [PSCustomObject]@{ Subsystem='Network';      Pattern='\[net\] DHCP lease applied' },
    [PSCustomObject]@{ Subsystem='ACPI';         Pattern='\[acpi\] AML inventory: dsdt=\d+ bytes' },
    [PSCustomObject]@{ Subsystem='fw_cfg';       Pattern='\[fw_cfg\] QEMU fw_cfg interface present' }
)

$globalResults = @()
$serialA = Join-Path $LogDir "serial.m26a.log"
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
    Write-Host "[final] WARN: $serialA missing (M26A skipped or failed) -- global sweep skipped" -ForegroundColor Yellow
    foreach ($g in $globalChecks) {
        $globalResults += [PSCustomObject]@{ Subsystem=$g.Subsystem; Verdict='SKIP'; Pattern=$g.Pattern }
    }
}

# --- summary --------------------------------------------------------
Write-Host ""
Write-Host "================== M26 PER-PHASE RESULTS ==================" -ForegroundColor Cyan
$results | Select-Object Phase, Verdict, Elapsed, Desc | Format-Table -AutoSize -Wrap | Out-String | Write-Host

Write-Host "================== GLOBAL REGRESSION SWEEP ==================" -ForegroundColor Cyan
$globalResults | Select-Object Subsystem, Verdict | Format-Table -AutoSize | Out-String | Write-Host

# --- known limitations ----------------------------------------------
Write-Host "================== KNOWN LIMITATIONS ==================" -ForegroundColor Cyan
@(
    "M26B: hub nesting depth 1 (validated); deeper trees untested but should work.",
    "M26C: hot-plug uses idle-loop polling (~10 ms latency) instead of MSI port-status-change interrupt.",
    "M26D: input routing prefers the most-recently-attached HID; no per-window/grab arbitration.",
    "M26E: removal-while-mounted leaves the mount point in EIO-only mode until explicit unmount; no auto-cleanup.",
    "M26F: tone playback is verb/DMA-validated only -- audible output not asserted (QEMU 'none' backend).",
    "M26F: codec mixer/jack-detect not exercised; only DAC + first PIN walked.",
    "M26G: no AML interpreter -- a PNP0C0A heuristic hit reports 'present, charge unknown'. Real _BIF/_BST evaluation is M27+ work.",
    "M26G: single battery only; multi-battery laptops will report only the first declared device.",
    "M26G: heuristic byte scan won't match _HID = EISAID(`"PNP0C0A`") packed-encoded form (rare in practice)."
) | ForEach-Object { Write-Host "  - $_" }

# --- QEMU validation commands ---------------------------------------
Write-Host ""
Write-Host "================== QEMU VALIDATION COMMANDS ==================" -ForegroundColor Cyan
Write-Host "  Run all M26 per-phase scripts (each is independent):"
Write-Host "    pwsh tests/test_m26a.ps1     # peripheral harness"
Write-Host "    pwsh tests/test_m26b.ps1     # USB hub"
Write-Host "    pwsh tests/test_m26c.ps1     # USB hot-plug"
Write-Host "    pwsh tests/test_m26d.ps1     # USB HID"
Write-Host "    pwsh tests/test_m26e.ps1     # USB mass storage"
Write-Host "    pwsh tests/test_m26f.ps1     # HD Audio"
Write-Host "    pwsh tests/test_m26g.ps1     # ACPI battery (absent + fw_cfg mock)"
Write-Host ""
Write-Host "  Or run this aggregator:"
Write-Host "    pwsh tests/test_m26_final.ps1            # all phases + global sweep"
Write-Host "    pwsh tests/test_m26_final.ps1 -Build     # rebuild kernel ISO first"
Write-Host "    pwsh tests/test_m26_final.ps1 -Skip M26B # skip a phase by tag"
Write-Host ""
Write-Host "  Notable QEMU snippets used by the per-phase scripts:"
Write-Host "    USB hub:        -device qemu-xhci,id=usb0 -device usb-hub,bus=usb0.0,id=hub1"
Write-Host "                    -device usb-kbd,bus=hub1.0 -device usb-mouse,bus=hub1.0"
Write-Host "    Hot-plug:       -qmp tcp:127.0.0.1:4444,server=on,wait=off  (for device_add/del)"
Write-Host "    HD Audio:       -audiodev none,id=hda -device intel-hda,id=hda0"
Write-Host "                    -device hda-duplex,audiodev=hda"
Write-Host "    Battery mock:   -fw_cfg name=opt/tobyos/battery_mock,string=state=charging;percent=75;design=50000;remaining=37500;rate=1500"

# --- real-hardware test checklist ----------------------------------
Write-Host ""
Write-Host "================== REAL-HARDWARE TEST CHECKLIST ==================" -ForegroundColor Cyan
@(
    "[ ] Boot the ISO on a bare-metal x86_64 PC with USB ports + a USB hub.",
    "[ ] Run 'devlist'. Confirm at least one xhci/uhci/ehci PCI bus + 1 hub + 1 HID + 1 BLK.",
    "[ ] Run 'usbtest hub'. Hub descriptor + downstream port count must match the physical hub.",
    "[ ] Run 'usbtest hotplug'. Unplug + replug a stick; expect ATTACH then DETACH events in the drain output.",
    "[ ] Plug in a USB keyboard + mouse together. Type into the shell, click in the GUI -- both must work.",
    "[ ] Plug in a FAT32-formatted USB stick. Run 'usbtest storage'. mount/RW/unmount must all PASS.",
    "[ ] Yank the USB stick WHILE mounted -- expect a [WARN] line and EIO from subsequent reads, not a panic.",
    "[ ] On a laptop with HD Audio: run 'audiotest'. Controller PASS + at least 1 codec record expected.",
    "[ ] On a laptop with a battery: run 'batterytest'. Expect 'PNP0C0A detected in DSDT...' + selftest PASS.",
    "[ ] On a desktop without a battery: run 'batterytest'. Expect SKIP path, no FAIL.",
    "[ ] Unplug the USB hub mid-session. xhci_service_port_changes should clean up downstream slots without leaks.",
    "[ ] PS/2 fallback: boot on a machine with a PS/2 keyboard. Confirm '[ps2_kbd]' chars increment as you type."
) | ForEach-Object { Write-Host "  $_" }

# --- final verdict --------------------------------------------------
# PowerShell quirk: `Where-Object` returns a scalar (not an array) when
# exactly one item matches, so `.Count` can be 1 OR an enumerated
# property of the single object. Wrap in @() to force array context.
Write-Host ""
$failedPhases = @($results       | Where-Object { $_.Verdict -eq 'FAIL' }).Count
$failedGlobal = @($globalResults | Where-Object { $_.Verdict -eq 'FAIL' }).Count
$totalFail    = $failedPhases + $failedGlobal

if ($totalFail -eq 0) {
    Write-Host "================== M26 FINAL: PASS ==================" -ForegroundColor Green
    Write-Host "All M26 phases passed and no global regression detected." -ForegroundColor Green
    exit 0
} else {
    Write-Host "================== M26 FINAL: FAIL ==================" -ForegroundColor Red
    Write-Host "$failedPhases per-phase failure(s) + $failedGlobal global regression(s)." -ForegroundColor Red
    exit 1
}
