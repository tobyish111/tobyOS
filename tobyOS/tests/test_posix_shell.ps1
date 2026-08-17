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
    'POSIXSH: arith-cmp=1-0-1-1',
    'POSIXSH: arith-logic=0-1-1',
    'POSIXSH: strip-prefix=world.txt',
    'POSIXSH: strip-prefix-greedy=txt',
    'POSIXSH: strip-suffix=hello.world',
    'POSIXSH: strip-suffix-greedy=hello',
    'POSIXSH: elif-ok',
    'POSIXSH: negate=0',
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
    'POSIXSH: getopts-group-b-bee-3',
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
    'POSIXSH: echo-n-ok',
    'POSIXSH: echo-e-tab',
    'POSIXSH: pos10=j-pos11=k',
    'POSIXSH: break2=1a',
    'POSIXSH: cont2=1a2a',
    'POSIXSH: xtrace-on',
    'POSIXSH: errexit-status=1',
    'POSIXSH: nounset-status=',
    'POSIXSH: dollar-dash=ex',
    'POSIXSH: tilde-user=/home/root',
    'POSIXSH: ifs-split=3-a-b-c',
    'export POSIXSH_EP="hello"',
    'POSIXSH: setdash-count=0',
    "trap -- '' INT",
    'POSIXSH: test-t-ok',
    'POSIXSH: test-L-ok',
    'POSIXSH: ansic=hello',
    'POSIXSH: star-ifs=a,b,c',
    'POSIXSH: cmd-p-ok',
    'POSIXSH: arith-var=30',
    'POSIXSH: arith-tern=42',
    'POSIXSH: arith-asgn=8-8',
    'POSIXSH: noglob=*.txt',
    'POSIXSH: case-or-ok',
    'POSIXSH: unset-f-ok',
    'POSIXSH: arith-hex=255',
    'POSIXSH: arith-oct=8',
    'POSIXSH: arith-comma=3',
    'POSIXSH: arith-preinc=6-6',
    'POSIXSH: arith-postinc=6-7',
    'POSIXSH: forimpl-hello',
    'POSIXSH: forimpl-world',
    'POSIXSH: printf-b=tab',
    'POSIXSH: noglob2=*',
    'POSIXSH: done',
    'POSIXSH: PASS',

    # --- POSIX conformance, second pass ---
    'POSIXSH: while=3',
    'POSIXSH: order=1',
    'POSIXSH: alias2-ok',
    'POSIXSH: qmark-status=2',
    'POSIXSH: nocolon-dash=alt',
    'POSIXSH: empty-dash=[] empty-colon=[alt]',
    'POSIXSH: nest=deep',
    'POSIXSH: bits=2-7-5-16-16',
    'POSIXSH: bitnot=-1',
    'POSIXSH: math=-3-3-1',
    'POSIXSH: arith-cs=6',
    'POSIXSH: arith-brace=14',
    'POSIXSH: test-nz-ok',
    'POSIXSH: test-int-ok',
    'POSIXSH: test-logic-ok',
    'POSIXSH: test-paren-ok',
    'POSIXSH: test-file-ok',
    'POSIXSH: cd-dash=/ oldpwd=/data',
    'POSIXSH: qat-[a b]',
    'POSIXSH: qat-[c]',
    'POSIXSH: impl-[d e]',
    'POSIXSH: impl-[f]',
    'POSIXSH: empty-at-ok',
    'POSIXSH: split-[x]',
    'POSIXSH: split-[y]',
    'POSIXSH: split-[z]',
    'POSIXSH: nosplit-[x y z]',
    'POSIXSH: star-count=2 at-count=2',
    'POSIXSH: pf1=42|str|Z|ff|10|   ab|ab   |ab|%',
    'POSIXSH: pf2=42|+7| 7|0xff|010|00042|   42|42   |',
    'POSIXSH: pf-reuse=a.POSIXSH: pf-reuse=b.POSIXSH: pf-reuse=c.',
    'POSIXSH: pff=[3.141590][3.14][    3.14][3.14    ][0.001][100.0]',
    'POSIXSH: pfe=[1.234500e+03][1.23e+03][1.200000E-04]',
    'POSIXSH: pfg=[0.0001][100000][1.23457e+06][1.23e+03]',
    'POSIXSH: glob-cls /data/posixsh_a.txt /data/posixsh_b.txt',
    'POSIXSH: glob-neg /data/posixsh_a.txt',
    'POSIXSH: case-q-ok',
    'POSIXSH: case-cls-ok',
    'POSIXSH: case-neg-ok',
    'POSIXSH: case-sub=yes',
    'POSIXSH: plain-if',
    'POSIXSH: plain-else',
    'POSIXSH: plain-elif',
    'POSIXSH: nested-if',
    'POSIXSH: fd3-line',
    'POSIXSH: fd4-read=POSIXSH: fd3-line',
    'POSIXSH: fd5-ok',
    'POSIXSH: noclobber-status=1',
    'POSIXSH: clobber-force',
    'POSIXSH: wr-one',
    'POSIXSH: wr-two',
    'POSIXSH: wr-three',
    'POSIXSH: while-read-status=0',
    'POSIXSH: forredir-1',
    'POSIXSH: forredir-2',
    'POSIXSH: ifredir',
    'POSIXSH: caseredir',
    'POSIXSH: read-noopt=atb',
    'POSIXSH: read-r=a\tb',
    'POSIXSH: dot-2-alpha',
    'POSIXSH: fn-2-inner1-inner2',
    'POSIXSH: fn-restore=2-outer1',
    'POSIXSH: umask=0022',
    'POSIXSH: ulimit=unlimited',
    'set +o allexport',
    'set +o xtrace',
    'POSIXSH: killl-name=KILL num=15',
    'POSIXSH: lineno1=1',
    'POSIXSH: lineno2=2',
    "trap -- 'echo POSIXSH: trap-hup' HUP",
    'POSIXSH: pipe-status=1',
    'POSIXSH: pipe-status2=0',

    # multi-line scripts (newline as command separator)
    'POSIXSH: ml-if',
    'POSIXSH: ml-for-a',
    'POSIXSH: ml-for-b',
    'POSIXSH: ml-while=2',
    'POSIXSH: ml-fn-hello',
    'POSIXSH: ml-case',
    'POSIXSH: ml-cont=joined',
    'POSIXSH: ml-bt=backtick',
    'POSIXSH: ml-substatus=1',
    'POSIXSH: ml-arg0=/data/px_multi.sh',
    'POSIXSH: ml-done',
    'POSIXSH: multi-status=0',
    'POSIXSH: deep=2000',

    # compound commands inside a list, recursion, and the remaining semantics
    'POSIXSH: enter-2',
    'POSIXSH: enter-1',
    'POSIXSH: enter-0',
    'POSIXSH: rec-0',
    'POSIXSH: rec-1',
    'POSIXSH: rec-2',
    'POSIXSH: bare-exit=1',
    'POSIXSH: ifs-empty=3-[a]-[]-[b]',
    'POSIXSH: catpipe=hello',
    'POSIXSH: dash-heredoc',
    'POSIXSH: quoted-heredoc $HOME',

    # pattern words, case quoting, errexit scope, set -n, pipe into a function
    'POSIXSH: defsub=made',
    'POSIXSH: patvar=abc',
    'POSIXSH: mixq=a''b''c esc=a"b',
    'POSIXSH: case-quoted-ok',
    'POSIXSH: cs-split=x y',
    'POSIXSH: cs-noq=x  y',
    'POSIXSH: errexit-fn=1',
    'POSIXSH: errexit-if-ok',
    'POSIXSH: noexec-status=0',
    'POSIXSH: pl-p1',
    'POSIXSH: pl-p2',

    # any run of blanks between `;` and the reserved word after it
    'POSIXSH: sp-for-a',
    'POSIXSH: sp-for-b',
    'POSIXSH: sp-if',
    'POSIXSH: sp-while=2',
    'POSIXSH: sp-elif',
    'POSIXSH: sp-else',
    'POSIXSH: sp-until',

    # ulimit stores real soft/hard values; getconf/pathchk/logname/newgrp/fc
    'POSIXSH: ul-soft=512 hard=512',
    'POSIXSH: ul-hard2=256',
    'POSIXSH: ul-raise=1',
    'POSIXSH: ul-over=1',
    'POSIXSH: getconf=200809',
    'POSIXSH: getconf-bad=1',
    'POSIXSH: pathchk-ok=0',
    'POSIXSH: pathchk-p=1',
    'POSIXSH: newgrp=1',
    'POSIXSH: fc-target',
    'POSIXSH: fc-replaced',

    # set -n reads and syntax-checks without executing
    'POSIXSH: noexec-clean=0',
    'POSIXSH: noexec-bad=2',
    'POSIXSH: noexec-good=0',
    'POSIXSH: noexec-quote=2',
    "sh: syntax error: 'if' without matching 'fi'",
    'sh: syntax error: unterminated single quote'
)

# The harness echoes every command line it drives ("[shell-test] $ ..."), and
# those echoes contain the very sentinels the commands are supposed to PRINT.
# Matching against them turns a broken feature into a green check -- that is
# how `POSIXSH: if-ok` and `POSIXSH: alias-ok` passed while `if ...; fi` and
# alias expansion were both broken. Assert against real output only.
$out = ($txt -split "`n" | Where-Object { $_ -notmatch '\[shell-test\] \$ ' }) -join "`n"

$missing = @()
foreach ($pat in $required) {
    if ($out -notmatch [regex]::Escape($pat)) { $missing += $pat }
}
if ($txt -match '(?m)^POSIXSH: if-bad') { $missing += 'unexpected POSIXSH: if-bad' }
if ($txt -match '(?m)^POSIXSH: elif-bad') { $missing += 'unexpected POSIXSH: elif-bad' }
if ($txt -match '(?m)^POSIXSH: case-bad') { $missing += 'unexpected POSIXSH: case-bad' }
if ($txt -match '(?m)^POSIXSH: exit-bad') { $missing += 'unexpected POSIXSH: exit-bad' }
if ($txt -match '(?m)^POSIXSH: trap-reset-bad') { $missing += 'unexpected POSIXSH: trap-reset-bad' }
if ($txt -match '(?m)^POSIXSH: case-wild') { $missing += 'unexpected POSIXSH: case-wild' }
if ($out -match '(?m)^POSIXSH: ml-if-bad') { $missing += 'unexpected POSIXSH: ml-if-bad' }
if ($out -match '(?m)^POSIXSH: ml-case-bad') { $missing += 'unexpected POSIXSH: ml-case-bad' }
if ($out -match '(?m)^POSIXSH: case-quoted-bad') { $missing += 'unexpected POSIXSH: case-quoted-bad' }
if ($out -match '(?m)^POSIXSH: errexit-fn-bad') { $missing += 'unexpected POSIXSH: errexit-fn-bad' }
if ($out -match '(?m)^POSIXSH: never') { $missing += 'unexpected POSIXSH: never' }
if ($txt -match '(?m)^POSIXSH: loop-later') { $missing += 'unexpected POSIXSH: loop-later' }
if ($txt -match '(?m)^POSIXSH: return-bad') { $missing += 'unexpected POSIXSH: return-bad' }
if ($txt -match '(?m)^POSIXSH: subshell-alias-bad') { $missing += 'unexpected POSIXSH: subshell-alias-bad' }
if ($txt -match '(?m)^POSIXSH: subshell-function-bad') { $missing += 'unexpected POSIXSH: subshell-function-bad' }
if ($txt -match '(?m)^POSIXSH: subshell-exit-bad') { $missing += 'unexpected POSIXSH: subshell-exit-bad' }
if ($txt -match '(?m)^POSIXSH: errexit-bad') { $missing += 'unexpected POSIXSH: errexit-bad' }
if ($out -notmatch 'posixsh_a\.txt' -or $out -notmatch 'posixsh_b\.txt') {
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
