# test_m34.ps1 -- boot-driven validation suite for milestone 34.
#
# Usage:
#     pwsh -File test_m34.ps1
#
# Prereqs: the kernel must already be built with -DPKG_M34_SELFTEST.
# The convenience wrapper at the bottom rebuilds for you if you pass
# -Build. The script:
#
#   1. starts QEMU headless with debug serial captured into serial.log
#   2. waits for the "[m34-selftest] milestone 34 end" marker (or a
#      hard timeout)
#   3. greps the log for individual "[m34?-selftest]" PASS/FAIL lines
#      and prints a tidy summary
#   4. exits 0 iff every reported step is PASS and the final
#      "failures=0 -- PASS" wrapper line is present
#
# Mirrors the layout of test_pkg_boot.ps1 (m17 boot harness) so the
# same operator muscle memory applies. Sub-tests for M34B/C/D/E will
# get appended as the corresponding sub-milestones land.

[CmdletBinding()]
param(
    [switch] $Build,
    [int]    $TimeoutSec = 60,
    [string] $Qemu       = "C:\Program Files\qemu\qemu-system-x86_64.exe"
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Qemu)) {
    Write-Error "qemu-system-x86_64.exe not found at $Qemu"
    exit 2
}

if ($Build) {
    Write-Host "[m34] rebuilding kernel with -DPKG_M34_SELFTEST..."
    & make m34test 2>&1 | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "make m34test failed (exit $LASTEXITCODE)"
        exit 2
    }
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

$proc = Start-Process -FilePath $Qemu `
                      -ArgumentList $qemuArgs `
                      -PassThru `
                      -RedirectStandardError "qemu.stderr.log"
Write-Host "[m34] qemu pid=$($proc.Id) booting (timeout=${TimeoutSec}s)..."

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$done     = $false
$tail     = "[m34-selftest] milestone 34 end"
while ((Get-Date) -lt $deadline -and -not $done) {
    Start-Sleep -Milliseconds 500
    if (Test-Path debug.log) {
        $content = Get-Content debug.log -Raw -ErrorAction SilentlyContinue
        if ($content -and $content.Contains($tail)) {
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
    Write-Host "[m34] TIMEOUT: never saw '$tail' in debug.log" `
        -ForegroundColor Red
    Write-Host "[m34] last 40 debug.log lines:"
    Get-Content debug.log -Tail 40 -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "    $_" }
    exit 1
}

$log = Get-Content debug.log -ErrorAction SilentlyContinue
if (-not $log) {
    Write-Error "debug.log is empty"
    exit 1
}

Write-Host ""
Write-Host "=== M34 self-test breadcrumbs ==="
$lines = $log | Select-String -Pattern '\[m34[a-g]?-selftest\]'
$totalSteps = 0
$passSteps  = 0
$failSteps  = 0
foreach ($l in $lines) {
    $txt = $l.Line.Trim()
    Write-Host "    $txt"
    if ($txt -match 'step \d+: PASS')      { $totalSteps++; $passSteps++ }
    elseif ($txt -match 'step \d+: FAIL')  { $totalSteps++; $failSteps++ }
}

Write-Host ""
Write-Host "=== verdict ==="
$verdict = $log | Select-String `
    -Pattern '\[m34-selftest\] milestone 34 end \(failures=(\d+)\) -- (PASS|FAIL)'
if ($verdict) {
    $line     = $verdict.Line.Trim()
    Write-Host "    $line"
    $failures = [int]$verdict.Matches[0].Groups[1].Value
    $verdict_w = $verdict.Matches[0].Groups[2].Value
    Write-Host ("    sub-steps: total={0} pass={1} fail={2}" `
        -f $totalSteps, $passSteps, $failSteps)
    if ($verdict_w -eq 'PASS' -and $failures -eq 0 -and $failSteps -eq 0) {
        Write-Host "[m34] OVERALL: PASS" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "[m34] OVERALL: FAIL" -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "    no verdict line found in debug.log" -ForegroundColor Red
    exit 1
}
