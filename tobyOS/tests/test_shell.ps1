# Headless boot + QMP send-key driver for the shell.
# Spawns QEMU with debugcon + serial logged to files, opens a QMP TCP
# socket, types a script of commands, then dumps debug.log so we can
# verify the shell.


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$serialLog = Join-Path $LogDir "serial.log"
$debugLog  = Join-Path $LogDir "debug.log"
$qemuLogF  = Join-Path $LogDir "qemu.log"
$qemuStderr = Join-Path $LogDir "qemu.stderr.log"

$ErrorActionPreference = 'Stop'

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -ErrorAction SilentlyContinue $serialLog, $debugLog, $qemuLogF, $qemuStderr

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$qemuArgs = @(
    "-cdrom", "tobyOS.iso",
    "-drive", "file=disk.img,format=raw,if=ide,index=0,media=disk",
    "-smp", "4",
    "-serial", ("file:" + $serialLog),
    "-debugcon", ("file:" + $debugLog),
    "-d", "cpu_reset,guest_errors", "-D", $qemuLogF,
    "-no-reboot", "-no-shutdown",
    "-display", "none",
    "-qmp", "tcp:127.0.0.1:4445,server,nowait"
)

$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru -RedirectStandardError $qemuStderr
Write-Host "qemu pid = $($proc.Id), waiting for boot..."
# Limine BIOS auto-boot timeout is ~5s; wait long enough that our kernel
# is up and the shell prompt is on screen before we start typing.
Start-Sleep -Seconds 8

# Connect to QMP
$client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 4445)
$stream = $client.GetStream()
$reader = New-Object System.IO.StreamReader($stream)
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true

function Send-Json($obj) {
    $j = $obj | ConvertTo-Json -Compress -Depth 10
    $writer.WriteLine($j)
    Start-Sleep -Milliseconds 50
}

# Read greeting + send capabilities negotiation
$null = $reader.ReadLine()
Send-Json @{ execute = 'qmp_capabilities' }
$null = $reader.ReadLine()

function Send-Key($qcode) {
    $payload = @{
        execute   = 'send-key'
        arguments = @{
            keys = @( @{ type = 'qcode'; data = $qcode } )
        }
    }
    Send-Json $payload
    $null = $reader.ReadLine()
}

function Send-Combo($qcodes) {
    $payload = @{
        execute   = 'send-key'
        arguments = @{
            keys = @($qcodes | ForEach-Object { @{ type = 'qcode'; data = $_ } })
        }
    }
    Send-Json $payload
    $null = $reader.ReadLine()
}

function Send-Text($text) {
    foreach ($ch in $text.ToCharArray()) {
        switch ($ch) {
            ' '  { Send-Key 'spc';                       continue }
            '.'  { Send-Key 'dot';                       continue }
            ','  { Send-Key 'comma';                     continue }
            '-'  { Send-Key 'minus';                     continue }
            '/'  { Send-Key 'slash';                     continue }
            '\'  { Send-Key 'backslash';                 continue }
            '_'  { Send-Combo @('shift', 'minus');       continue }
            '|'  { Send-Combo @('shift', 'backslash');   continue }
            default { Send-Key ([string]$ch).ToLower() }
        }
    }
}

function Send-Line($text) {
    Send-Text $text
    Send-Key 'ret'
    Start-Sleep -Milliseconds 200
}

# Drive the shell
Send-Line 'help'
Send-Line 'about'
Send-Line 'ps'
Send-Line 'ls /bin'
Send-Line 'cat /etc/motd'
Send-Line 'run /bin/hello'
Send-Line 'echo first run done'
Send-Line 'run /bin/hello'
Send-Line 'echo second run done'
Send-Line 'run /bin/hello'
Send-Line 'echo third run done'
Send-Line 'ps'
Send-Line 'run /bin/bad'
Send-Line 'echo kernel survived user-mode fault'
Send-Line 'run /bin/hello'
Send-Line 'echo run still works after fault'
Send-Line 'run /etc/motd'
Send-Line 'cat /nope'
Send-Line 'cpus'

# ---- milestone 6: writable FS exercises ----
Send-Line 'mounts'
Send-Line 'ls /data'
Send-Line 'mkdir /data/docs'
Send-Line 'touch /data/docs/empty.txt'
Send-Line 'write /data/docs/hello.txt hello from tobyfs'
Send-Line 'ls /data'
Send-Line 'ls /data/docs'
Send-Line 'cat /data/docs/hello.txt'
Send-Line 'echo end of milestone 6 read-back'
# Error paths -- kernel must NOT panic.
Send-Line 'write /etc/motd nope ramfs is read-only'
Send-Line 'mkdir /etc/should-fail'
Send-Line 'rm /data/docs/empty.txt'
Send-Line 'rm /data/docs'
Send-Line 'echo expected-fail rm-non-empty-dir above'
Send-Line 'rm /data/docs/hello.txt'
Send-Line 'rm /data/docs'
Send-Line 'ls /data'

# ---- milestone 7: pipes + shell pipelines ----
Send-Line 'ls /bin'
Send-Line 'run /bin/echo'
Send-Line 'echo hello | cat'
Send-Line 'echo this is a longer message | cat'
Send-Line 'echo first | cat'
Send-Line 'echo second | cat'
Send-Line 'echo end of milestone 7 pipeline tests'
# Error path: pipe-prefixed missing program; kernel must NOT panic.
Send-Line 'echo x | nope-program'
Send-Line 'echo done after bad pipeline'
# Multi-arg + back-to-back pipelines to confirm no leak / no orphans.
Send-Line 'mem'
Send-Line 'echo a | cat'
Send-Line 'echo b | cat'
Send-Line 'echo c | cat'
Send-Line 'mem'
Send-Line 'ps'
Start-Sleep -Seconds 2

# Ask QMP nicely to quit so debugcon flushes to disk.
Send-Json @{ execute = 'quit' }
Start-Sleep -Milliseconds 400
$writer.Close()
$client.Close()
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
Start-Sleep -Milliseconds 300

Write-Host "=== debug.log ==="
Get-Content $debugLog -ErrorAction SilentlyContinue
