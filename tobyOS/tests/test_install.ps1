# test_install.ps1 -- milestone 20 end-to-end installer smoketest.
#
# Phase 1: builds the kernel with -DINSTALL_M20_SELFTEST, wipes the
#          target disk to a blank 16 MiB, boots the live tobyOS.iso
#          headless, and waits for "[m20-selftest] SUCCESS" on
#          serial. The selftest runs the exact same installer_run()
#          the shell `install --yes` command would -- it just fires
#          at boot so we don't have to drive the GUI with QMP
#          keystrokes. Success means:
#             * install.img flashed into sectors 0..8191 of disk.img
#             * tobyfs formatted starting at sector 8192
#             * /mnt/welcome.txt readable from the new filesystem
#
# Phase 2: boots disk.img directly (no -cdrom, -boot c) and waits
#          for the kernel's /data mount message + the idempotent
#          "SKIP: /data/welcome.txt already present" from the
#          selftest -- proves the disk is HDD-bootable from Limine's
#          MBR stage1 and that the installed /data survived reboot.
#
# Run from the tobyOS project root. Requires MSYS2 UCRT64 on PATH
# (make, clang, ld.lld, xorriso).

# NOTE: we deliberately leave $ErrorActionPreference at its default
# ('Continue') because native tools in the build pipeline (xorriso,
# clang, ld.lld) scribble their normal progress messages to stderr,
# and PowerShell 5's 'Stop' mode treats those as terminating errors
# when combined with '2>&1'. We check $LASTEXITCODE and the serial
# log ourselves to decide pass/fail.

$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

if ($PSVersionTable.PSVersion.Major -ge 7) {
    $PSNativeCommandUseErrorActionPreference = $false
}

# Make sure the MSYS2 UCRT64 toolchain (make, clang, ld.lld, xorriso)
# is discoverable no matter how the script was launched. If you use
# a non-default MSYS2 install path, override by setting MSYSTEM_DIR
# in the environment before running this script.
$msysDir = if ($env:MSYSTEM_DIR) { $env:MSYSTEM_DIR } else { 'C:\msys64' }
if (Test-Path "$msysDir\usr\bin\make.exe") {
    $env:Path = "$msysDir\ucrt64\bin;$msysDir\usr\bin;" + $env:Path
} elseif (-not (Get-Command make -ErrorAction SilentlyContinue)) {
    throw "make not on PATH and no MSYS2 install found at $msysDir (set MSYSTEM_DIR)"
}

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$serialLog = Join-Path $LogDir "serial.log"
$debugLog  = Join-Path $LogDir "debug.log"

function Stop-AllQemu {
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue |
        Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

function Wait-ForPattern($path, $pattern, $timeoutSec, $failPattern) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if (Test-Path $path) {
            $log = Get-Content -Raw $path -ErrorAction SilentlyContinue
            if ($log -match $pattern)     { return 'match' }
            if ($failPattern -and
                $log -match $failPattern) { return 'fail' }
        }
    }
    return 'timeout'
}

# ------------------------------------------------------------------
# Build: m20test kernel + fresh 16 MiB disk
# ------------------------------------------------------------------
Stop-AllQemu
Remove-Item -ErrorAction SilentlyContinue `
    $serialLog, $debugLog, (Join-Path $LogDir "qemu.log"), (Join-Path $LogDir "qemu.stderr.log")

Write-Host "[build] make m20test -- kernel with -DINSTALL_M20_SELFTEST"
$p = Start-Process -FilePath (Get-Command make.exe).Source `
        -ArgumentList 'm20test' `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput 'm20test.build.log' `
        -RedirectStandardError  'm20test.build.err'
if ($p.ExitCode -ne 0) {
    Get-Content m20test.build.log, m20test.build.err -ErrorAction SilentlyContinue |
        Select-Object -Last 30 | Out-Host
    throw "make m20test failed (exit=$($p.ExitCode))"
}
Get-Content m20test.build.log -ErrorAction SilentlyContinue |
    Select-Object -Last 3 | Out-Host

Write-Host "[build] make wipe-disk -- fresh 16 MiB blank disk.img"
$p = Start-Process -FilePath (Get-Command make.exe).Source `
        -ArgumentList 'wipe-disk' `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput 'm20test.wipe.log' `
        -RedirectStandardError  'm20test.wipe.err'
if ($p.ExitCode -ne 0) {
    Get-Content m20test.wipe.log, m20test.wipe.err -ErrorAction SilentlyContinue |
        Select-Object -Last 30 | Out-Host
    throw "make wipe-disk failed (exit=$($p.ExitCode))"
}
Get-Content m20test.wipe.log -ErrorAction SilentlyContinue |
    Select-Object -Last 3 | Out-Host

# ------------------------------------------------------------------
# Phase 1: live ISO -> m20-selftest runs installer at boot
# ------------------------------------------------------------------
Write-Host ""
Write-Host "=== PHASE 1: live ISO install ==="

$args1 = @(
    "-cdrom", "tobyOS.iso",
    "-drive", "file=disk.img,format=raw,if=ide,index=0,media=disk",
    "-boot", "d",
    "-smp", "4",
    "-serial", ("file:" + $serialLog),
    "-debugcon", ("file:" + $debugLog),
    "-display", "none",
    "-no-reboot"
)
$p1 = Start-Process -FilePath $qemu -ArgumentList $args1 `
          -PassThru -WindowStyle Hidden
Write-Host "[phase1] qemu pid=$($p1.Id), waiting for [m20-selftest] SUCCESS..."

$res = Wait-ForPattern -path $serialLog `
        -pattern '\[m20-selftest\] SUCCESS' `
        -failPattern '\[m20-selftest\] FAIL' `
        -timeoutSec 120
switch ($res) {
    'match'   { Write-Host "[phase1] PASS: install completed" }
    'fail'    { Write-Host "[phase1] FAIL: installer reported failure"; }
    'timeout' { Write-Host "[phase1] FAIL: timeout after 120s" }
}
Stop-AllQemu

Write-Host ""
Write-Host "--- phase1 installer/selftest lines ---"
Select-String -Path $serialLog `
    -Pattern '\[m20-selftest\]|\[installer\]|\[ata\]' `
    -ErrorAction SilentlyContinue |
    Select-Object -Last 25 |
    ForEach-Object { $_.Line } | Out-Host

if ($res -ne 'match') { throw "phase1 failed ($res)" }

# ------------------------------------------------------------------
# Phase 2: boot the installed disk directly (no -cdrom, -boot c)
# ------------------------------------------------------------------
Write-Host ""
Write-Host "=== PHASE 2: boot installed disk (no CD-ROM) ==="
Remove-Item -ErrorAction SilentlyContinue $serialLog, $debugLog

$args2 = @(
    "-drive", "file=disk.img,format=raw,if=ide,index=0,media=disk",
    "-boot", "c",
    "-smp", "4",
    "-serial", ("file:" + $serialLog),
    "-debugcon", ("file:" + $debugLog),
    "-display", "none",
    "-no-reboot"
)
$p2 = Start-Process -FilePath $qemu -ArgumentList $args2 `
          -PassThru -WindowStyle Hidden
Write-Host "[phase2] qemu pid=$($p2.Id), waiting for /data mount + selftest skip..."

# We expect to see BOTH:
#   - kernel mounting /data at LBA 8192 (proves MBR->limine->kernel
#     chain AND tobyfs at the installed offset)
#   - selftest SKIPping because /data/welcome.txt already exists
#     (proves the install is persistent AND idempotent)
$r1 = Wait-ForPattern -path $serialLog `
        -pattern 'mounted installed /data at LBA 8192' `
        -failPattern 'panic:|unrecoverable' `
        -timeoutSec 60
$r2 = Wait-ForPattern -path $serialLog `
        -pattern '\[m20-selftest\] SKIP' `
        -failPattern 'panic:' `
        -timeoutSec 60

Stop-AllQemu

Write-Host ""
Write-Host "--- phase2 relevant lines ---"
Select-String -Path $serialLog `
    -Pattern '\[m20-selftest\]|\[boot\] /data|\[boot\] mounted|\[tobyfs\] mounted|\[settings\]' `
    -ErrorAction SilentlyContinue |
    Select-Object -Last 20 |
    ForEach-Object { $_.Line } | Out-Host

if ($r1 -ne 'match') { throw "phase2: /data not mounted from installed layout ($r1)" }
if ($r2 -ne 'match') { throw "phase2: selftest did not see installed /data ($r2)" }

Write-Host ""
Write-Host "=== ALL GREEN: milestone 20 installer works end-to-end ==="
Write-Host "    - tobyOS.iso boots live + runs installer"
Write-Host "    - disk.img is now HDD-bootable (Limine MBR stage1)"
Write-Host "    - /data survives reboot at LBA 8192"
