# test_m35.ps1 -- boot-driven driver for the per-phase Milestone 35
# selftests (m35a..m35f). The M35G compatibility-validation suite has
# its own driver, test_m35g.ps1.
#
# Usage:
#     pwsh -File test_m35.ps1
#
# What it does:
#   1. boots tobyOS headless under QEMU using the kernel that was
#      already built by `make m35test` (does NOT rebuild on its own)
#   2. captures serial + debug logs
#   3. waits for the LAST "[m35*-selftest] milestone 35* end" verdict
#      line (-Phases governs how many we expect)
#   4. echoes every step + verdict for the PR/CI log
#   5. exits 0 iff every observed verdict is PASS and we saw at least
#      one of them
#
# Mirrors test_m34.ps1 / test_m34g.ps1 so muscle memory carries.


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

[CmdletBinding()]
param(
    [int]      $TimeoutSec = 90,
    [string]   $Qemu       = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    [string[]] $Phases     = @('m35a','m35b','m35c','m35d','m35e','m35f'),
    # When set, attach build/vblk_test.img as a modern virtio-blk-pci
    # device so the M35B selftest can do a real sentinel read against
    # vblk0 instead of skipping. Default = on.
    [switch]   $WithVirtioBlk = $true,
    # When set, attach a qemu-xhci controller plus usb-kbd, usb-mouse,
    # and usb-storage so the M35C selftest's "live" steps run instead
    # of skipping. Default = on. Toggling off reproduces a stripped
    # config (which still PASSes -- the synthetic checks always run).
    [switch]   $WithUsbStack = $true
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Qemu)) {
    Write-Error "qemu-system-x86_64.exe not found at $Qemu"
    exit 2
}

$serialLog = Join-Path $LogDir "serial.log"
$debugLog  = Join-Path $LogDir "debug.log"
$qemuLogF  = Join-Path $LogDir "qemu.log"
$qemuStderr = Join-Path $LogDir "qemu.stderr.log"

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue |
    Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -ErrorAction SilentlyContinue $serialLog, $debugLog, $qemuLogF, $qemuStderr

$qemuArgs = @(
    "-cdrom",       "tobyOS.iso",
    "-drive",       "file=disk.img,format=raw,if=ide,index=0,media=disk",
    "-smp",         "4",
    "-serial",      ("file:" + $serialLog),
    "-debugcon",    ("file:" + $debugLog),
    "-d",           "cpu_reset,guest_errors", "-D", $qemuLogF,
    "-no-reboot",   "-no-shutdown",
    "-display",     "none"
)

if ($WithVirtioBlk) {
    $vblk = "build/vblk_test.img"
    if (-not (Test-Path $vblk)) {
        # Create a 4 MiB raw image with a 16-byte ASCII sentinel at LBA 0.
        # Stays in lockstep with the Makefile's `build/vblk_test.img` rule
        # so test_m35.ps1 can be invoked without `make run-virtio-blk`.
        New-Item -ItemType Directory -Force -Path build | Out-Null
        $fs = [System.IO.File]::Open($vblk, 'Create', 'Write')
        try {
            $fs.SetLength(4MB)
            $bytes = [System.Text.Encoding]::ASCII.GetBytes("TOBYM35B-VBLK0`0")
            $fs.Position = 0
            $fs.Write($bytes, 0, $bytes.Length)
        } finally { $fs.Dispose() }
        Write-Host "[m35] created $vblk (4 MiB, sentinel @LBA0)"
    }
    $qemuArgs += @(
        "-drive",  "file=$vblk,format=raw,if=none,id=vblk0",
        "-device", "virtio-blk-pci,drive=vblk0,disable-legacy=on,disable-modern=off"
    )
}

if ($WithUsbStack) {
    # Need a small FAT32 image for usb-storage. Reuse build/usbstick.img
    # (created by `make usb-stick-img`) if present; otherwise fall back
    # to vblk_test.img -- usb-storage only cares that the backing file
    # exists; the M35C selftest doesn't read FS contents off it.
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
                      -RedirectStandardError $qemuStderr
Write-Host "[m35] qemu pid=$($proc.Id) booting (timeout=${TimeoutSec}s)..."

# Wait for the LAST configured phase's end marker. Phase A always runs
# first; later phases extend the right edge.
$lastPhase = $Phases[-1]
$tail      = "[$lastPhase-selftest] milestone"
$deadline  = (Get-Date).AddSeconds($TimeoutSec)
$done      = $false
while ((Get-Date) -lt $deadline -and -not $done) {
    Start-Sleep -Milliseconds 500
    if (Test-Path $debugLog) {
        $content = Get-Content $debugLog -Raw -ErrorAction SilentlyContinue
        if ($content -and $content.Contains("$tail 35") -and
            ($content -match [regex]::Escape("$tail") + ".+end")) {
            $done = $true
        }
    }
}

if (-not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force
}
Start-Sleep -Milliseconds 200

if (-not $done) {
    Write-Host ""
    Write-Host "[m35] TIMEOUT: never saw end marker for phase '$lastPhase'" `
        -ForegroundColor Red
    Write-Host "[m35] last 60 debug.log lines:"
    Get-Content $debugLog -Tail 60 -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "    $_" }
    exit 1
}

$log = Get-Content $debugLog -ErrorAction SilentlyContinue
if (-not $log) {
    Write-Error "debug log is empty ($debugLog)"
    exit 1
}

Write-Host ""
Write-Host "=== M35 selftest breadcrumbs ==="
$totalSteps = 0
$passSteps  = 0
$failSteps  = 0
$skipSteps  = 0
foreach ($p in $Phases) {
    $tagPattern = "\[$p-selftest\]"
    $lines = $log | Select-String -Pattern $tagPattern
    foreach ($l in $lines) {
        $txt = $l.Line.Trim()
        Write-Host "    $txt"
        if ($txt -match 'step \d+: PASS')               { $totalSteps++; $passSteps++ }
        elseif ($txt -match 'step \d+: FAIL')           { $totalSteps++; $failSteps++ }
        elseif ($txt -match 'step \d+: SKIPPED_')       { $skipSteps++ }
    }
}

Write-Host ""
Write-Host "=== per-phase verdicts ==="
$allPass = $true
foreach ($p in $Phases) {
    # The verdict trailer is either:
    #   "(failures=N total=M) -- PASS"
    # or, when a phase tracks SKIPPED steps:
    #   "(failures=N total=M skipped=K) -- PASS"
    $verdict = $log | Select-String `
        -Pattern ("\[" + $p + "-selftest\] milestone .+ end \(failures=(\d+) total=(\d+)(?: skipped=(\d+))?\) -- (PASS|FAIL)")
    if (-not $verdict) {
        Write-Host "    $p : MISSING" -ForegroundColor Red
        $allPass = $false
        continue
    }
    $fails   = [int]$verdict.Matches[0].Groups[1].Value
    $total   = [int]$verdict.Matches[0].Groups[2].Value
    $skipped = if ($verdict.Matches[0].Groups[3].Value) {
                   [int]$verdict.Matches[0].Groups[3].Value
               } else { 0 }
    $w       = $verdict.Matches[0].Groups[4].Value
    # A phase counts as PASS if its driver said PASS AND there were no
    # failed steps. total==0 is acceptable when EVERY step was skipped
    # (e.g. m35b on a build without a virtio-blk attached) -- as long as
    # the skip count makes it explicit.
    $hadCoverage = ($total -gt 0) -or ($skipped -gt 0)
    if ($w -eq 'PASS' -and $fails -eq 0 -and $hadCoverage) {
        Write-Host ("    {0} : PASS  (total={1} fail={2} skipped={3})" `
            -f $p, $total, $fails, $skipped) -ForegroundColor Green
    } else {
        Write-Host ("    {0} : FAIL  (total={1} fail={2} skipped={3} verdict={4})" `
            -f $p, $total, $fails, $skipped, $w) -ForegroundColor Red
        $allPass = $false
    }
}

Write-Host ""
Write-Host ("    breadcrumbs: total={0} pass={1} fail={2} skipped={3}" `
    -f $totalSteps, $passSteps, $failSteps, $skipSteps)
if ($allPass -and $failSteps -eq 0 -and ($totalSteps -gt 0 -or $skipSteps -gt 0)) {
    Write-Host "[m35] OVERALL: PASS" -ForegroundColor Green
    exit 0
} else {
    Write-Host "[m35] OVERALL: FAIL" -ForegroundColor Red
    exit 1
}
