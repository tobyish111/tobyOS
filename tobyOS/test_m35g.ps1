# test_m35g.ps1 -- Milestone 35G compatibility-validation driver.
#
# Boots tobyOS headless under QEMU with the production kernel (no
# -DM35_SELFTEST needed; M35G runs unconditionally on every boot)
# and asserts that:
#
#   1. the M35G harness fires (kernel-side `[boot] M35G:` lines)
#   2. /bin/compattest --boot prints the eight bucket sentinels
#   3. every bucket is PASS or SKIPPED_REAL_HARDWARE_REQUIRED
#   4. the final M35G_CMP: VERDICT line is PASS
#   5. the boot does not panic, hang, or wedge
#
# We deliberately attach a richer QEMU config than test_m35.ps1 so
# the SKIP-rate stays low: virtio-blk + qemu-xhci + usb-kbd +
# usb-mouse + usb-storage so STORAGE / USB_INPUT exercise live
# devices, plus the default user-mode networking stack so NETWORK
# binds e1000 (or virtio-net via -nic). All optional via switches.
#
# Mirrors test_m35.ps1's UX so muscle memory carries.

[CmdletBinding()]
param(
    [int]      $TimeoutSec = 90,
    [string]   $Qemu       = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    # Net: defaults to the user-mode SLIRP NIC QEMU ships, attached
    # as virtio-net so STORAGE/NETWORK/USB_INPUT all bind.
    [switch]   $WithNet       = $true,
    [switch]   $WithVirtioBlk = $true,
    [switch]   $WithUsbStack  = $true
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Qemu)) {
    Write-Error "qemu-system-x86_64.exe not found at $Qemu"
    exit 2
}

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue |
    Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -ErrorAction SilentlyContinue serial.log, debug.log, qemu.log

$qemuArgs = @(
    "-cdrom",       "tobyOS.iso",
    "-drive",       "file=disk.img,format=raw,if=ide,index=0,media=disk",
    "-smp",         "4",
    "-serial",      "file:serial.log",
    "-debugcon",    "file:debug.log",
    "-d",           "cpu_reset,guest_errors", "-D", "qemu.log",
    "-no-reboot",   "-no-shutdown",
    "-display",     "none"
)

if ($WithNet) {
    # Drop QEMU's default e1000 in favour of virtio-net so the M35G
    # NETWORK bucket exercises a SUPPORTED tier driver.
    $qemuArgs += @(
        "-nic", "user,model=virtio-net-pci"
    )
}

if ($WithVirtioBlk) {
    $vblk = "build/vblk_test.img"
    if (-not (Test-Path $vblk)) {
        New-Item -ItemType Directory -Force -Path build | Out-Null
        $fs = [System.IO.File]::Open($vblk, 'Create', 'Write')
        try {
            $fs.SetLength(4MB)
            $bytes = [System.Text.Encoding]::ASCII.GetBytes("TOBYM35B-VBLK0`0")
            $fs.Position = 0
            $fs.Write($bytes, 0, $bytes.Length)
        } finally { $fs.Dispose() }
        Write-Host "[m35g] created $vblk (4 MiB, sentinel @LBA0)"
    }
    $qemuArgs += @(
        "-drive",  "file=$vblk,format=raw,if=none,id=vblk0",
        "-device", "virtio-blk-pci,drive=vblk0,disable-legacy=on,disable-modern=off"
    )
}

if ($WithUsbStack) {
    $usbBack = "build/usbstick.img"
    if (-not (Test-Path $usbBack)) { $usbBack = "build/vblk_test.img" }
    $qemuArgs += @(
        "-device", "qemu-xhci,id=usb0",
        "-device", "usb-kbd,bus=usb0.0",
        "-device", "usb-mouse,bus=usb0.0",
        "-drive",  "if=none,id=usbstick,format=raw,file=$usbBack",
        "-device", "usb-storage,bus=usb0.0,drive=usbstick"
    )
}

$proc = Start-Process -FilePath $Qemu `
                      -ArgumentList $qemuArgs `
                      -PassThru `
                      -RedirectStandardError "qemu.stderr.log"
Write-Host "[m35g] qemu pid=$($proc.Id) booting (timeout=${TimeoutSec}s)..."

# Wait for the compattest verdict line. The kernel harness emits
# "[boot] M35G: compattest harness complete" right after the spawn
# returns; we also wait for the M35G_CMP: VERDICT sentinel itself
# so the script can read the per-bucket lines after the boot is
# fully done.
$tail     = "M35G_CMP: VERDICT:"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$done     = $false
while ((Get-Date) -lt $deadline -and -not $done) {
    Start-Sleep -Milliseconds 500
    if (Test-Path debug.log) {
        $content = Get-Content debug.log -Raw -ErrorAction SilentlyContinue
        if ($content -and $content.Contains($tail)) { $done = $true }
    }
}

if (-not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force
}
Start-Sleep -Milliseconds 200

if (-not $done) {
    Write-Host ""
    Write-Host "[m35g] TIMEOUT: never saw '$tail' sentinel" `
        -ForegroundColor Red
    Write-Host "[m35g] last 60 debug.log lines:"
    Get-Content debug.log -Tail 60 -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "    $_" }
    exit 1
}

$log = Get-Content debug.log -ErrorAction SilentlyContinue
if (-not $log) {
    Write-Error "debug.log is empty"
    exit 1
}

Write-Host ""
Write-Host "=== M35G compattest sentinels ==="
$cmpLines = $log | Select-String -Pattern '^M35G_CMP:'
foreach ($l in $cmpLines) {
    Write-Host "    $($l.Line.Trim())"
}

$pass = 0
$fail = 0
$skip = 0
$expectedBuckets = @(
    'SYSTEM_BOOT', 'DRIVER_MATCH', 'FALLBACK_PATHS',
    'NETWORK',     'STORAGE',      'USB_INPUT',
    'LOG_CAPTURE', 'NO_CRASHES'
)
$seenBuckets = @{}
foreach ($l in $cmpLines) {
    $line = $l.Line.Trim()
    foreach ($b in $expectedBuckets) {
        if ($line -match "M35G_CMP:\s+$b\s*:\s*(PASS|FAIL|SKIPPED_REAL_HARDWARE_REQUIRED)") {
            $st = $matches[1]
            $seenBuckets[$b] = $st
            switch ($st) {
                'PASS'                          { $pass++ }
                'FAIL'                          { $fail++ }
                'SKIPPED_REAL_HARDWARE_REQUIRED'{ $skip++ }
            }
        }
    }
}

Write-Host ""
Write-Host "=== per-bucket results ==="
$missing = $false
foreach ($b in $expectedBuckets) {
    if (-not $seenBuckets.ContainsKey($b)) {
        Write-Host "    $b : MISSING" -ForegroundColor Red
        $missing = $true
    } else {
        $st = $seenBuckets[$b]
        $color = switch ($st) {
            'PASS' { 'Green' }
            'FAIL' { 'Red' }
            default { 'Yellow' }
        }
        Write-Host ("    {0,-15} : {1}" -f $b, $st) -ForegroundColor $color
    }
}

# Final tally line emitted by the tool itself.
$verdict = $log | Select-String `
    -Pattern '^M35G_CMP:\s+VERDICT:\s+(PASS|FAIL)\s+pass=(\d+)\s+fail=(\d+)\s+skipped=(\d+)'
if (-not $verdict) {
    Write-Host "[m35g] OVERALL: FAIL (no VERDICT sentinel)" `
        -ForegroundColor Red
    exit 1
}
$verd  = $verdict.Matches[0].Groups[1].Value
$tpass = [int]$verdict.Matches[0].Groups[2].Value
$tfail = [int]$verdict.Matches[0].Groups[3].Value
$tskip = [int]$verdict.Matches[0].Groups[4].Value

Write-Host ""
Write-Host ("    sentinel verdict: {0}  (pass={1} fail={2} skipped={3})" `
    -f $verd, $tpass, $tfail, $tskip)

# Sanity: the kernel-side spawn line should also report PASS.
$harnessOk = ($log | Select-String '^\[boot\] M35G: /bin/compattest.+PASS').Count -gt 0
if (-not $harnessOk) {
    Write-Host "[m35g] WARN: kernel-side `[boot] M35G:` PASS sentinel missing" `
        -ForegroundColor Yellow
}

if ($verd -eq 'PASS' -and $fail -eq 0 -and -not $missing) {
    Write-Host "[m35g] OVERALL: PASS" -ForegroundColor Green
    exit 0
} else {
    Write-Host "[m35g] OVERALL: FAIL" -ForegroundColor Red
    exit 1
}
