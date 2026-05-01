# test_pkg_upgrade.ps1 -- Headless smoke test for milestone 17.
#
# Since the GUI login (milestone 14+) owns the keyboard at boot, we
# can't reliably drive the kernel shell over QMP. Instead we build
# the kernel with -DPKG_M17_SELFTEST (via `make m17test`), which makes
# kmain run pkg_m17_selftest() right after pkg_init(). That self-test
# exercises the entire upgrade flow:
#
#   - pkg_install_name("helloapp")        installs v1.0.0
#   - pkg_update                          shows UPGRADE row for 1.1.0
#   - pkg_upgrade_one("helloapp")         bumps to 1.1.0 (with backup)
#   - pkg_upgrade_one again               "already up-to-date" no-op
#   - pkg_rollback("helloapp")            restores 1.0.0 from .bak
#   - pkg_upgrade_all                     re-applies 1.1.0
#   - pkg_remove                          cleanup
#
# Each step prints "[m17-selftest] step N: OK" so we can grep the log
# to confirm. Run from the tobyOS directory.


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$serialLog = Join-Path $LogDir "serial.log"
$debugLog  = Join-Path $LogDir "debug.log"
$qemuLogF  = Join-Path $LogDir "qemu.log"
$qemuStderr = Join-Path $LogDir "qemu.stderr.log"

$ErrorActionPreference = 'Continue'

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -ErrorAction SilentlyContinue $serialLog, $debugLog, $qemuLogF, $qemuStderr

# Fresh disk so previous runs don't leave helloapp + .bak around.
Remove-Item -ErrorAction SilentlyContinue disk.img

Write-Host "[m17] building self-test kernel (make m17test)..."
$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:Path
& "C:\msys64\usr\bin\make.exe" m17test 2>&1 |
    Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" }

# After build we still need a fresh disk image.
& "C:\msys64\usr\bin\make.exe" disk 2>&1 | Out-Null

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
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

$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru -RedirectStandardError $qemuStderr
Write-Host "qemu pid = $($proc.Id), running self-test (~10s)..."
Start-Sleep -Seconds 10
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
Start-Sleep -Milliseconds 400

$log = Get-Content $serialLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "=== [m17-selftest] step results ==="
$steps = $log | Select-String -Pattern '\[m17-selftest\] (step \d+|begin|end)'
if ($steps) {
    $steps | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
} else {
    Write-Host "  (no [m17-selftest] lines found -- did `make m17test` run?)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== install/upgrade/rollback breadcrumbs ==="
$log | Select-String -Pattern '\[pkg\] (install|upgrade|rollback|backup)' |
    ForEach-Object { Write-Host "  $($_.Line.Trim())" }

Write-Host ""
Write-Host "=== version transition check ==="
# Look for the "X -> Y" line that prints during pkg upgrade.
$log | Select-String -Pattern "1\.0\.0 -> 1\.1\.0|1\.1\.0 -> 1\.0\.0|upgraded helloapp|rolling back" |
    ForEach-Object { Write-Host "  $($_.Line.Trim())" }

Write-Host ""
Write-Host "=== pass/fail summary ==="
$ok   = ($log | Select-String -Pattern '\[m17-selftest\] step \d+: OK').Count
$bad  = ($log | Select-String -Pattern '\[m17-selftest\] step \d+: FAIL').Count
$end  = $log  | Select-String -Pattern '\[m17-selftest\] milestone 17 end'
Write-Host "  OK steps : $ok"
Write-Host "  FAIL steps: $bad"
if ($end) { Write-Host "  $($end.Line.Trim())" }

Write-Host ""
Write-Host "=== panic/assert check ==="
# Filter out benign "fault"-substring matches from the unrelated
# settings/users 'no such file or directory' default-write messages.
$bad = $log | Select-String -Pattern 'panic|PANIC|assert|page_fault|gp fault'
if ($bad) {
    Write-Host "  FOUND ERRORS:" -ForegroundColor Red
    $bad | ForEach-Object { Write-Host "    $($_.Line.Trim())" }
} else {
    Write-Host "  clean -- no panic/assert/fault in serial.log"
}
