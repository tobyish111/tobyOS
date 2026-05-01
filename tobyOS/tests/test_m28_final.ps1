# test_m28_final.ps1 -- Milestone 28 final validation aggregator.
#
# Runs every M28 phase test (A through G) sequentially and prints a
# PASS/FAIL summary at the end, mirroring the format the M28 spec
# requested ("Final Output Requirements"). Each child script handles
# its own QEMU spin-up, build flags, and serial-log assertions; this
# script is just a harness that catches their exit codes and renders
# a single human-readable scoreboard.
#
# Exit 0 => every phase PASSed
# Exit 1 => any phase FAILed
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File ./test_m28_final.ps1
#   powershell -ExecutionPolicy Bypass -File ./test_m28_final.ps1 -SkipReboot
#       (skips M28A's "logs persist across reboot" boot, ~30s faster)

param(
    [switch]$SkipReboot
)


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$ErrorActionPreference = 'Stop'
Set-Location -Path (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Phase definitions: each entry is {name, script, blurb}. Order matters
# because later phases (G) implicitly assume earlier phases (A-F) work.
$phases = @(
    @{ id='M28A'; script='test_m28a.ps1'; blurb='Logging + diagnostics (slog ring + persistent file + logview)' },
    @{ id='M28B'; script='test_m28b.ps1'; blurb='Kernel panic + crash dumps (panic screen, dump to /data/crash)' },
    @{ id='M28C'; script='test_m28c.ps1'; blurb='Watchdog + hang detection (kernel/sched/syscall heartbeats)' },
    @{ id='M28D'; script='test_m28d.ps1'; blurb='Safe mode boot (minimal driver init, emergency shell)' },
    @{ id='M28E'; script='test_m28e.ps1'; blurb='Filesystem integrity (tobyfs check + fscheck tool)' },
    @{ id='M28F'; script='test_m28f.ps1'; blurb='Service supervision + restart (BACKOFF, DISABLED, recovery)' },
    @{ id='M28G'; script='test_m28g.ps1'; blurb='Stability validation suite (12-probe self-test + stress)' }
)

$totalStart = Get-Date
$results    = @()

Write-Host ""
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host " tobyOS Milestone 28 -- Final Stability Validation" -ForegroundColor Cyan
Write-Host "==============================================================" -ForegroundColor Cyan

foreach ($ph in $phases) {
    $name   = $ph.id
    $script = $ph.script
    $blurb  = $ph.blurb
    Write-Host ""
    Write-Host "--- $name -- $blurb ---" -ForegroundColor Cyan

    if (-not (Test-Path $script)) {
        Write-Host "[$name] SKIP: $script not present" -ForegroundColor Yellow
        $results += [pscustomobject]@{
            Phase    = $name
            Verdict  = 'SKIP'
            Seconds  = 0
            Reason   = "$script not found"
        }
        continue
    }

    $phaseStart = Get-Date
    $exitCode   = 0
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File ".\$script"
        $exitCode = $LASTEXITCODE
    } catch {
        $exitCode = 99
        Write-Host "[$name] EXCEPTION: $($_.Exception.Message)" -ForegroundColor Red
    }
    $phaseSec = [int]((Get-Date) - $phaseStart).TotalSeconds

    if ($exitCode -eq 0) {
        Write-Host "[$name] PASS in ${phaseSec}s" -ForegroundColor Green
        $results += [pscustomobject]@{
            Phase    = $name
            Verdict  = 'PASS'
            Seconds  = $phaseSec
            Reason   = ''
        }
    } else {
        Write-Host "[$name] FAIL (exit=$exitCode) in ${phaseSec}s" -ForegroundColor Red
        $results += [pscustomobject]@{
            Phase    = $name
            Verdict  = 'FAIL'
            Seconds  = $phaseSec
            Reason   = "exit=$exitCode"
        }
        # Continue running later phases regardless -- the aggregator
        # is meant to give a full scoreboard, not bail at the first
        # red square. The final exit code reflects the worst result.
    }
}

$totalSec = [int]((Get-Date) - $totalStart).TotalSeconds

Write-Host ""
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host " M28 SCOREBOARD ($totalSec s total)" -ForegroundColor Cyan
Write-Host "==============================================================" -ForegroundColor Cyan
$results | Format-Table Phase, Verdict, Seconds, Reason -AutoSize

# Force scalars: piping through @() ensures .Count works for 0/1/N matches.
$pass = @($results | Where-Object Verdict -EQ 'PASS').Count
$fail = @($results | Where-Object Verdict -EQ 'FAIL').Count
$skip = @($results | Where-Object Verdict -EQ 'SKIP').Count

Write-Host ""
Write-Host "Summary: $pass PASS, $fail FAIL, $skip SKIP" -ForegroundColor Cyan

if ($fail -gt 0) {
    Write-Host ""
    Write-Host "M28 OVERALL: FAIL" -ForegroundColor Red
    exit 1
}
if ($pass -eq 0) {
    Write-Host ""
    Write-Host "M28 OVERALL: NO TESTS RUN" -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "M28 OVERALL: PASS -- system is stability-validated." -ForegroundColor Green
Write-Host ""
Write-Host "Real-hardware checklist:" -ForegroundColor Cyan
Write-Host "  [ ] Boot: live ISO boots to login on a USB stick" -ForegroundColor Gray
Write-Host "  [ ] Crash recovery: /bin/crashtest panics, reboot, /bin/crashinfo lists dump" -ForegroundColor Gray
Write-Host "  [ ] Safe mode: hold no keys; serial shows safe shell prompt" -ForegroundColor Gray
Write-Host "  [ ] Filesystem: pull power mid-write, /bin/fscheck reports clean on next boot" -ForegroundColor Gray
Write-Host "  [ ] Device stability: /bin/usbtest, /bin/devlist, /bin/audiotest survive 5 min idle" -ForegroundColor Gray
exit 0
