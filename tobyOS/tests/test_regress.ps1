# test_regress.ps1 -- run a single make target headless and report.
#
# Usage:  pwsh -File tests/test_regress.ps1 -Target run-virtio-gpu [-Wait 18]   (cwd = tobyOS root)
#
# Steps:
#   1. taskkill any leftover QEMU + sleep so Windows releases the
#      serial.log file handle (otherwise make's `rm -f` fails).
#   2. Delete previous logs ourselves.
#   3. Spawn make in the background.
#   4. Sleep -Wait seconds.
#   5. taskkill QEMU again to unblock make's `cat` tail.
#   6. Wait for the spawned process to exit so file handles release.
#   7. Grep serial.log for the regression-success markers and print a
#      pass/fail line.
#
# Exit code: 0 on pass, 1 on fail. Output is intentionally short so
# multiple invocations stack up nicely in a regression sweep.

param(
    [Parameter(Mandatory=$true)] [string] $Target,
    [int] $Wait = 18
)


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$serialLog = Join-Path $LogDir "serial.log"
$makeOut   = Join-Path $LogDir "make_out.log"
$makeErr   = Join-Path $LogDir "make_err.log"

$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:Path

function Stop-Qemu {
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 1500
}

Stop-Qemu
Remove-Item -Force -ErrorAction SilentlyContinue `
    $serialLog, (Join-Path $LogDir "debug.log"), (Join-Path $LogDir "qemu.log"), `
    (Join-Path $LogDir "net.pcap"), $makeOut, $makeErr

$proc = Start-Process -FilePath "make" -ArgumentList $Target `
    -NoNewWindow -PassThru `
    -RedirectStandardOutput $makeOut `
    -RedirectStandardError $makeErr

Start-Sleep -Seconds $Wait
Stop-Qemu

if (-not $proc.WaitForExit(8000)) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $serialLog)) {
    Write-Host "[$Target] FAIL: serial log absent ($serialLog)"
    exit 1
}

$log = Get-Content -Raw $serialLog -ErrorAction SilentlyContinue
if (-not $log) {
    Write-Host "[$Target] FAIL: serial log empty"
    exit 1
}

# Success markers required for "GUI login window reached":
#   - "[gui] window manager ready"  (gfx + gui both up)
#   - "title='tobyOS login'"        (login window actually created)
$gfxOk   = $log -match '\[gui\] window manager ready'
$loginOk = $log -match "title='tobyOS login'"

if ($gfxOk -and $loginOk) {
    Write-Host "[$Target] PASS  (gfx ready + login window)"
    exit 0
}
$why = @()
if (-not $gfxOk)   { $why += "no [gui] window manager ready" }
if (-not $loginOk) { $why += "no tobyOS login window" }
Write-Host "[$Target] FAIL: $($why -join '; ')"
exit 1
