# test_posix_shell.ps1 -- boot-driven POSIX shell compatibility smoke.
#
# Build with:
#   make posixshtest
#
# Or let this driver rebuild first:
#   powershell -ExecutionPolicy Bypass -File tests\test_posix_shell.ps1 -Rebuild

param(
    [switch]$Rebuild
)

$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$serialLog = Join-Path $LogDir "serial.log"
$debugLog  = Join-Path $LogDir "debug.log"
$qemuLogF  = Join-Path $LogDir "qemu.log"

$ErrorActionPreference = 'Stop'

if ($Rebuild) {
    $bash = "C:\msys64\usr\bin\bash.exe"
    if (-not (Test-Path $bash)) { Write-Error "MSYS2 bash not found at $bash"; exit 2 }
    $rootMsys = ($RepoRoot -replace '\\','/')
    if ($rootMsys -match '^([A-Za-z]):/(.*)$') {
        $rootMsys = '/' + $Matches[1].ToLower() + '/' + $Matches[2]
    }
    & $bash -lc "PATH=/ucrt64/bin:/usr/bin:`$PATH; cd '$rootMsys' && make posixshtest"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

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
Write-Host "[posixsh] qemu pid=$($proc.Id) (timeout=$TimeoutSec s)..."

$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    if (Test-Path $serialLog) {
        $t = Get-Content -Raw $serialLog
        if ($t -match 'POSIXSH: PASS') { break }
    }
    if ($proc.HasExited) { break }
    Start-Sleep -Milliseconds 200
}
if (-not $proc.HasExited) { $proc | Stop-Process -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 400

$txt = if (Test-Path $serialLog) { Get-Content -Raw $serialLog } else { "" }
$required = @(
    'POSIXSH: start',
    'POSIXSH: subst=ok',
    'POSIXSH: backtick=ok2',
    'POSIXSH: or-ok',
    'POSIXSH: and-ok',
    'POSIXSH: if-ok',
    'POSIXSH: for-alpha',
    'POSIXSH: for-beta',
    'POSIXSH: params count=3 first=one all=one two three',
    'POSIXSH: default=fallback',
    'POSIXSH: assign=setnow again=setnow',
    'POSIXSH: alt=alt',
    'POSIXSH: len=6',
    'POSIXSH: arith=7',
    'POSIXSH: alias-ok',
    'POSIXSH: func-one-2',
    'POSIXSH: case-ok',
    'POSIXSH: loop-keep',
    'POSIXSH: until-once',
    'POSIXSH: script-ok',
    'POSIXSH: heredoc-setnow',
    'POSIXSH: return-before',
    'POSIXSH: return-status=7',
    'POSIXSH: shc-label-arg1-1',
    'POSIXSH: fd1-file',
    'p_cat: open /data/posixsh_no_fd2:',
    'p_cat: open /data/posixsh_no_merge:',
    'p_cat: open /data/posixsh_order:',
    'POSIXSH: dup-order-status=1',
    'POSIXSH: close-fd-status=1',
    'POSIXSH: redir-only-ok',
    'POSIXSH: read-file-alpha-beta gamma',
    'POSIXSH: read-heredoc-one-two',
    'POSIXSH: builtin-pipe',
    'POSIXSH: function-pipe',
    'POSIXSH: read-pipe-status=0 var=unset-unset',
    'POSIXSH: pipe-cwd-/',
    'POSIXSH: bgpid=',
    'POSIXSH: wait-status=0',
    'POSIXSH: wait-bg',
    'POSIXSH: wait-all-status=0',
    'POSIXSH: wait-all-bg',
    'POSIXSH: special-assign=kept',
    'POSIX_TEMP=visible',
    'POSIXSH: temp-after=unset',
    'POSIXSH: readonly-status=1 value=locked',
    'POSIXSH: shift-1-shift-c',
    'POSIXSH: getopts-a-none-2',
    'POSIXSH: getopts-b-bee-4',
    'POSIXSH: getopts-done-4',
    'POSIXSH: getopts-group-a-none-1',
    'POSIXSH: getopts-group-b-bee-2',
    'POSIXSH: getopts-explicit-x-explicit-3',
    'POSIXSH: getopts-bad-?-z-0',
    'POSIXSH: getopts-missing-:-b-0',
    'export is a special shell builtin',
    'POSIXSH: exit-before',
    'POSIXSH: exit-status=9',
    'POSIXSH: trap-body',
    'POSIXSH: trap-exit',
    'POSIXSH: trap-status=0',
    'POSIXSH: trap-reset-ok',
    'trap is a special shell builtin',
    'POSIXSH: func-redir-one',
    'POSIXSH: func-redir-two',
    'POSIXSH: func-ext-redir',
    'POSIXSH: group-redir-one',
    'POSIXSH: group-redir-two',
    'POSIXSH: subshell-pwd-/data var-inside',
    'POSIXSH: subshell-trap',
    'POSIXSH: subshell-outer-pwd=/ var=unset',
    'POSIXSH: subshell-alias-isolated',
    'POSIXSH: subshell-function-isolated',
    'POSIXSH: subshell-readonly-status=0',
    'POSIXSH: subshell-exit-before',
    'POSIXSH: subshell-exit-status=6',
    'POSIXSH: subshell-redir-one',
    'POSIXSH: subshell-redir-two',
    'POSIXSH: eval-ok',
    'POSIXSH: colon-before',
    'POSIXSH: colon-after',
    'POSIXSH: done',
    'POSIXSH: PASS'
)

$missing = @()
foreach ($pat in $required) {
    if ($txt -notmatch [regex]::Escape($pat)) { $missing += $pat }
}
if ($txt -match '(?m)^POSIXSH: if-bad') { $missing += 'unexpected POSIXSH: if-bad' }
if ($txt -match '(?m)^POSIXSH: case-bad') { $missing += 'unexpected POSIXSH: case-bad' }
if ($txt -match '(?m)^POSIXSH: exit-bad') { $missing += 'unexpected POSIXSH: exit-bad' }
if ($txt -match '(?m)^POSIXSH: trap-reset-bad') { $missing += 'unexpected POSIXSH: trap-reset-bad' }
if ($txt -match '(?m)^POSIXSH: case-wild') { $missing += 'unexpected POSIXSH: case-wild' }
if ($txt -match '(?m)^POSIXSH: loop-later') { $missing += 'unexpected POSIXSH: loop-later' }
if ($txt -match '(?m)^POSIXSH: return-bad') { $missing += 'unexpected POSIXSH: return-bad' }
if ($txt -match '(?m)^POSIXSH: subshell-alias-bad') { $missing += 'unexpected POSIXSH: subshell-alias-bad' }
if ($txt -match '(?m)^POSIXSH: subshell-function-bad') { $missing += 'unexpected POSIXSH: subshell-function-bad' }
if ($txt -match '(?m)^POSIXSH: subshell-exit-bad') { $missing += 'unexpected POSIXSH: subshell-exit-bad' }
if ($txt -notmatch 'posixsh_a\.txt' -or $txt -notmatch 'posixsh_b\.txt') {
    $missing += 'glob expansion did not show both posixsh files'
}

if ($missing.Count -gt 0) {
    Write-Host "=== POSIX shell smoke: FAIL ==="
    $missing | ForEach-Object { Write-Host "missing/bad: $_" }
    if ($txt -match 'POSIXSH:') {
        ($txt -split "`n") | Where-Object { $_ -match 'POSIXSH:' }
    }
    exit 1
}

Write-Host "[posixsh] OVERALL: PASS"
exit 0
