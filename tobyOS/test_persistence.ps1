#  test_persistence.ps1 -- end-to-end milestone-6 persistence test.
#
#  Pass 1:
#     - wipe disk.img with mkfs (fresh FS)
#     - boot tobyOS, mkdir + write some files
#     - quit cleanly (so QEMU flushes its cache to disk.img)
#  Pass 2:
#     - boot the SAME disk.img with no setup
#     - cat the files back, verify the bytes survived

$ErrorActionPreference = 'Stop'

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$mkfs = "build/mkfs_tobyfs.exe"
$disk = "disk.img"
$iso  = "tobyOS.iso"

if (-not (Test-Path $mkfs)) {
    Write-Host "mkfs not built -- run 'make mkfs' first" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $iso)) {
    Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red
    exit 1
}

# ---- helpers reused from test_shell.ps1 ----

function Start-Tobyos($logSuffix) {
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    Remove-Item -ErrorAction SilentlyContinue "serial.$logSuffix.log", "debug.$logSuffix.log"
    $args = @(
        "-cdrom", $iso,
        "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
        "-smp", "4",
        "-serial", "file:serial.$logSuffix.log",
        "-debugcon", "file:debug.$logSuffix.log",
        "-d", "cpu_reset,guest_errors", "-D", "qemu.$logSuffix.log",
        "-no-reboot", "-no-shutdown",
        "-display", "none",
        "-qmp", "tcp:127.0.0.1:4445,server,nowait"
    )
    $p = Start-Process -FilePath $qemu -ArgumentList $args -PassThru -RedirectStandardError "qemu.$logSuffix.stderr.log"
    Write-Host "[pass:$logSuffix] qemu pid = $($p.Id), waiting for boot..."
    Start-Sleep -Seconds 8
    return $p
}

function Connect-Qmp {
    $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 4445)
    $stream = $client.GetStream()
    $reader = New-Object System.IO.StreamReader($stream)
    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.AutoFlush = $true
    function Send-Json-Local($obj) {
        $j = $obj | ConvertTo-Json -Compress -Depth 10
        $writer.WriteLine($j)
        Start-Sleep -Milliseconds 50
    }
    $null = $reader.ReadLine()
    Send-Json-Local @{ execute = 'qmp_capabilities' }
    $null = $reader.ReadLine()
    return @{ client = $client; reader = $reader; writer = $writer }
}

function Send-Json-To($conn, $obj) {
    $j = $obj | ConvertTo-Json -Compress -Depth 10
    $conn.writer.WriteLine($j)
    Start-Sleep -Milliseconds 50
}

function Send-Key-To($conn, $qcode) {
    Send-Json-To $conn @{
        execute   = 'send-key'
        arguments = @{ keys = @( @{ type = 'qcode'; data = $qcode } ) }
    }
    $null = $conn.reader.ReadLine()
}

function Send-Combo-To($conn, $qcodes) {
    Send-Json-To $conn @{
        execute   = 'send-key'
        arguments = @{ keys = @($qcodes | ForEach-Object { @{ type = 'qcode'; data = $_ } }) }
    }
    $null = $conn.reader.ReadLine()
}

function Send-Text-To($conn, $text) {
    foreach ($ch in $text.ToCharArray()) {
        switch ($ch) {
            ' '  { Send-Key-To $conn 'spc';                       continue }
            '.'  { Send-Key-To $conn 'dot';                       continue }
            ','  { Send-Key-To $conn 'comma';                     continue }
            '-'  { Send-Key-To $conn 'minus';                     continue }
            '/'  { Send-Key-To $conn 'slash';                     continue }
            '\'  { Send-Key-To $conn 'backslash';                 continue }
            '_'  { Send-Combo-To $conn @('shift', 'minus');       continue }
            default { Send-Key-To $conn ([string]$ch).ToLower() }
        }
    }
}

function Send-Line-To($conn, $text) {
    Send-Text-To $conn $text
    Send-Key-To $conn 'ret'
    Start-Sleep -Milliseconds 200
}

function Stop-Tobyos($conn, $proc) {
    Send-Json-To $conn @{ execute = 'quit' }
    Start-Sleep -Milliseconds 600
    $conn.writer.Close()
    $conn.client.Close()
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    Start-Sleep -Milliseconds 400
}

# ---- Pass 1: wipe + write ----

Write-Host ""
Write-Host "============== PASS 1: format + write =================" -ForegroundColor Cyan
Remove-Item -ErrorAction SilentlyContinue $disk
& $mkfs $disk
if ($LASTEXITCODE -ne 0) { Write-Host "mkfs failed"; exit 1 }

$proc = Start-Tobyos "pass1"
$conn = Connect-Qmp

Send-Line-To $conn 'mounts'
Send-Line-To $conn 'ls /data'
Send-Line-To $conn 'mkdir /data/docs'
Send-Line-To $conn 'write /data/docs/hello.txt persisted across reboot'
Send-Line-To $conn 'write /data/note.txt second file'
Send-Line-To $conn 'ls /data'
Send-Line-To $conn 'ls /data/docs'
Send-Line-To $conn 'cat /data/docs/hello.txt'
Send-Line-To $conn 'echo pass1 done -- shutting down to flush disk'
Start-Sleep -Seconds 1

Stop-Tobyos $conn $proc

Write-Host ""
Write-Host "=== debug.pass1.log (last 80 lines) ==="
Get-Content debug.pass1.log -ErrorAction SilentlyContinue -Tail 80

# ---- Pass 2: reboot, read back ----

Write-Host ""
Write-Host "============== PASS 2: reboot, read back ==============" -ForegroundColor Cyan
$proc = Start-Tobyos "pass2"
$conn = Connect-Qmp

Send-Line-To $conn 'mounts'
Send-Line-To $conn 'ls /data'
Send-Line-To $conn 'ls /data/docs'
Send-Line-To $conn 'cat /data/docs/hello.txt'
Send-Line-To $conn 'cat /data/note.txt'
Send-Line-To $conn 'echo PASS2 SUCCESS if both cats printed pass1 contents'
Start-Sleep -Seconds 1

Stop-Tobyos $conn $proc

Write-Host ""
Write-Host "=== debug.pass2.log (last 80 lines) ==="
Get-Content debug.pass2.log -ErrorAction SilentlyContinue -Tail 80
