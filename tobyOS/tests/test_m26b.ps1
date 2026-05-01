# test_m26b.ps1 -- M26B USB Hub Support validation.
#
# M26B adds:
#   * src/usb_hub.c (hub class driver: descriptor, port power, reset,
#     enumeration of downstream devices)
#   * xhci_attach_via_hub + xhci_configure_as_hub
#   * abi_dev_info hub_depth/hub_port + ABI_DEVT_BUS_HUB enumeration
#   * /bin/usbtest hub subcommand + boot harness lines
#
# This script boots tobyOS in QEMU with a topology that exercises
# every part of the new code path:
#
#   qemu-xhci  ->  usb-hub (8 ports)
#                    +-- usb-kbd
#                    +-- usb-mouse
#                    +-- usb-storage  (raw FAT32 backing file)
#
# It then waits for the boot harness sentinel, kills QEMU, and greps
# the captured serial.log for the M26B-specific PASS markers as well
# as the inherited M26A invariants (no panics, every queued userland
# tool exit-0).
#
# Exit 0 => PASS, exit 1 => FAIL (with the missing patterns listed).


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$usbStick = "build/usbstick.img"
$logTag   = "m26b"
$serial   = Join-Path $LogDir "serial.$logTag.log"
$debug    = Join-Path $LogDir "debug.$logTag.log"
$qemuLog  = Join-Path $LogDir "qemu.$logTag.log"
# We watch for the same sentinel as M26A -- the M26B markers all land
# upstream of (or in the same loop that produces) this line, so once
# it appears the boot sweep is provably done.
$bootSentinel = '\[boot\] M26A: userland .* PASS .* of \d+'
$timeoutSec   = 45

if (-not (Test-Path $iso))      { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk))     { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $usbStick)) { Write-Host "$usbStick missing -- run 'make usb_stick.img' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M26B: USB Hub Support ==============" -ForegroundColor Cyan

$qemuArgs = @(
    "-cdrom", $iso,
    "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
    "-drive", "if=none,id=usbstick,format=raw,file=$usbStick",
    "-smp", "4",
    "-netdev", "user,id=net0",
    "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
    # USB topology -- this is the M26B-specific bit. qemu-xhci is the
    # host controller, usb-hub adds an 8-port USB-2 hub on its root
    # port, and we attach kbd/mouse/storage *behind* the hub so every
    # downstream port walk must succeed for them to show up.
    # qemu-xhci exposes a single bus 'usb0.0'. usb-hub plugs into root
    # port 1 of that bus, and downstream usb devices address themselves
    # via port chains "1.<n>" rather than a separate bus name. (QEMU
    # does not expose the hub as its own pci bus -- so 'bus=hub1.0'
    # would error with "Bus 'hub1.0' not found".)
    "-device", "qemu-xhci,id=usb0",
    "-device", "usb-hub,bus=usb0.0,port=1,id=hub1",
    "-device", "usb-kbd,bus=usb0.0,port=1.1",
    "-device", "usb-mouse,bus=usb0.0,port=1.2",
    "-device", "usb-storage,bus=usb0.0,port=1.3,drive=usbstick",
    "-serial", "file:$serial",
    "-debugcon", "file:$debug",
    "-d", "cpu_reset,guest_errors", "-D", $qemuLog,
    "-no-reboot", "-display", "none"
)
$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
Write-Host "[m26b] qemu pid = $($proc.Id), watching $serial for boot sentinel..."

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
# These are split into three groups for diagnostics: kernel-side hub
# class probe, userland-visible hub records, and the M26A invariants
# we still must not regress.
$mustHave = @(
    # --- kernel-side hub class driver ---
    # usb_hub_probe() reached and the descriptor parsed correctly.
    '\[usb_hub\] slot \d+: HUB nports=\d+ char=0x[0-9a-fA-F]+ pwr2pwrgood=\d+ ms',
    # At least one downstream device was addressed (kbd/mouse/storage).
    '\[usb_hub\] slot \d+ port \d+: device connected, speed=\d+',
    # Aggregate per-hub summary.
    '\[usb_hub\] slot \d+: \d+/\d+ ports populated, \d+ attached',
    # devtest harness ran the new self-test as part of the boot sweep.
    '\[PASS\] usb_hub:',

    # --- shell builtins (M26B block in m26a_run_userland_tools) ---
    '--- M26B shell builtins ---',

    # --- userland devlist hub ---
    # The hub bus is enumerated through ABI_DEVT_BUS_HUB and printed
    # by tobydev_print_record. The driver column is "usb_hub" and the
    # extra column carries the slot/nports/up/attached/depth tuple.
    'usb_hub.*nports=\d+ up=\d+ attached=\d+ depth=\d+',

    # --- userland usbtest hub ---
    'INFO: usbtest hub: \d+ hub\(s\) registered',
    'INFO: usbtest hub: \d+ USB dev\(s\) total: \d+ root, [1-9]\d* behind hub',
    'INFO: usbtest hub: enumerating downstream devices --',
    'PASS: usbtest hub: hubs=\d+ ports=\d+ populated=\d+ attached=\d+',

    # --- M26A invariants (must not regress) ---
    '\[boot\] M26A: peripheral inventory \+ self-tests',
    '\[PASS\] xhci:',
    '\[boot\] M26A: /bin/devlist .*PASS\)',
    '\[boot\] M26A: /bin/usbtest .*PASS\)',
    # Self-referencing regex: "every tool that was queued passed".
    '\[boot\] M26A: userland (\d+) PASS / 0 FAIL / 0 missing of \1'
)
$missing = @()
foreach ($pat in $mustHave) {
    if ($txt -notmatch $pat) { $missing += $pat }
}

# === forbidden tokens ===
$forbidden = @('panic', 'page fault', 'KERNEL OOPS', '\[FAIL\] usb_hub', '\[FAIL\] xhci', '\[FAIL\] pci')
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M26B PASS -- USB hub probed, downstream devices enumerated, every tool exit-0." -ForegroundColor Green
    Write-Host ""
    Write-Host "Hub class probe / port walk lines from ${serial}:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '^\[usb_hub\]' | ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "usbtest hub output from ${serial}:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern 'usbtest hub' | ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "Final harness summary:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '\[boot\] M26A: userland .* PASS' | ForEach-Object { Write-Host "  $($_.Line)" }
    exit 0
} else {
    Write-Host "M26B FAIL" -ForegroundColor Red
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
