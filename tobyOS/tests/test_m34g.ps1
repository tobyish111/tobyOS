# test_m34g.ps1 -- boot-driven driver for the M34G integrated
# security validation suite.
#
# Usage:
#     pwsh -File test_m34g.ps1
#     pwsh -File test_m34g.ps1 -Build      # rebuild kernel first
#
# What it does:
#   1. (optionally) rebuild with `make sectest` so the kernel autoruns
#      sectest_run() at boot
#   2. boot tobyOS headless under QEMU, capturing serial + debug logs
#   3. wait for the canonical
#         [securitytest] OVERALL: PASS|FAIL pass=P fail=F skip=S total=T
#      marker
#   4. echo every "[securitytest] ..." breadcrumb so the PR/CI log has
#      the evidence trail
#   5. exit 0 iff verdict is PASS and at least one test ran
#
# Mirrors test_m34.ps1 so operator muscle memory carries.


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

[CmdletBinding()]
param(
    [switch] $Build,
    [int]    $TimeoutSec = 90,
    [string] $Qemu       = "C:\Program Files\qemu\qemu-system-x86_64.exe"
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

if ($Build) {
    Write-Host "[m34g] rebuilding kernel with -DSECTEST_AUTORUN..."
    & make sectest 2>&1 | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "make sectest failed (exit $LASTEXITCODE)"
        exit 2
    }
}

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

$proc = Start-Process -FilePath $Qemu `
                      -ArgumentList $qemuArgs `
                      -PassThru `
                      -RedirectStandardError $qemuStderr
Write-Host "[m34g] qemu pid=$($proc.Id) booting (timeout=${TimeoutSec}s)..."

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$done     = $false
$tail     = "[securitytest] OVERALL:"
while ((Get-Date) -lt $deadline -and -not $done) {
    Start-Sleep -Milliseconds 500
    if (Test-Path $debugLog) {
        $content = Get-Content $debugLog -Raw -ErrorAction SilentlyContinue
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
    Write-Host "[m34g] TIMEOUT: never saw '$tail' in debug.log" `
        -ForegroundColor Red
    Write-Host "[m34g] last 40 debug.log lines:"
    Get-Content $debugLog -Tail 40 -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "    $_" }
    exit 1
}

$log = Get-Content $debugLog -ErrorAction SilentlyContinue
if (-not $log) {
    Write-Error "debug log is empty ($debugLog)"
    exit 1
}

Write-Host ""
Write-Host "=== M34G securitytest breadcrumbs ==="
$lines = $log | Select-String -Pattern '\[securitytest\]'
$totalSteps = 0
$passSteps  = 0
$failSteps  = 0
$skipSteps  = 0
foreach ($l in $lines) {
    $txt = $l.Line.Trim()
    Write-Host "    $txt"
    if ($txt -match 'step \d+: PASS')      { $totalSteps++; $passSteps++ }
    elseif ($txt -match 'step \d+: FAIL')  { $totalSteps++; $failSteps++ }
    elseif ($txt -match 'step \d+: SKIP')  { $totalSteps++; $skipSteps++ }
}

Write-Host ""
Write-Host "=== verdict ==="
$verdict = $log | Select-String `
    -Pattern '\[securitytest\] OVERALL: (PASS|FAIL) pass=(\d+) fail=(\d+) skip=(\d+) total=(\d+)'
if ($verdict) {
    $line     = $verdict.Line.Trim()
    Write-Host "    $line"
    $w        = $verdict.Matches[0].Groups[1].Value
    $pass     = [int]$verdict.Matches[0].Groups[2].Value
    $fail     = [int]$verdict.Matches[0].Groups[3].Value
    $skip     = [int]$verdict.Matches[0].Groups[4].Value
    $total    = [int]$verdict.Matches[0].Groups[5].Value
    Write-Host ("    breadcrumbs: total={0} pass={1} fail={2} skip={3}" `
        -f $totalSteps, $passSteps, $failSteps, $skipSteps)
    if ($w -eq 'PASS' -and $fail -eq 0 -and $total -gt 0) {
        Write-Host "[m34g] OVERALL: PASS" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "[m34g] OVERALL: FAIL" -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "    no OVERALL line found in debug.log" -ForegroundColor Red
    exit 1
}
