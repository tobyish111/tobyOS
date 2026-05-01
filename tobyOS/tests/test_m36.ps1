# test_m36.ps1 -- boots tobyOS built with `make m36test` (-DM36_SELFTEST)
# and greps serial.log for "M36: PASS" from /bin/selfhosttest.


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$serialLog = Join-Path $LogDir "serial.log"
$debugLog  = Join-Path $LogDir "debug.log"
$qemuLogF  = Join-Path $LogDir "qemu.log"

$ErrorActionPreference = 'Stop'
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -ErrorAction SilentlyContinue $serialLog, $debugLog, $qemuLogF

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
if (-not (Test-Path $qemu)) { Write-Error "qemu not found at $qemu"; exit 2 }

$qemuArgs = @(
    "-cdrom", "tobyOS.iso",
    "-drive", "file=disk.img,format=raw,if=ide,index=0,media=disk",
    "-smp", "4",
    "-serial", ("file:" + $serialLog),
    "-debugcon", ("file:" + $debugLog),
    "-d", "cpu_reset,guest_errors", "-D", $qemuLogF,
    "-no-reboot", "-no-shutdown",
    "-display", "none"
)
$TimeoutSec = 120
$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
Write-Host "[m36] qemu pid=$($proc.Id) (timeout=$TimeoutSec s)..."
$dead = $false
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    if (Test-Path $serialLog) {
        $t = Get-Content -Raw $serialLog
        if ($t -match 'M36: PASS') { $dead = $true; break }
        if ($t -match 'M36: FAIL') { $dead = $true; break }
    }
    if ($proc.HasExited) { break }
    Start-Sleep -Milliseconds 200
}
if (-not $proc.HasExited) { $proc | Stop-Process -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 400

$txt = if (Test-Path $serialLog) { Get-Content -Raw $serialLog } else { "" }
if ($txt -notmatch 'M36: PASS') {
    Write-Host "=== m36: FAIL (no M36: PASS) ===" 
    if ($txt -match 'M36:') { ($txt -split "`n") | Where-Object { $_ -match 'M36:' } }
    exit 1
}
Write-Host "[m36] OVERALL: PASS"
exit 0
