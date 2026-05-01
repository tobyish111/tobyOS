# Headless boot + QMP-driven smoke test for milestone 16 (pkg manager).
#
# Boots tobyOS in QEMU headless, logs serial + debugcon to files, and
# drives the shell via QMP send-key to exercise:
#
#   pkg repo       -- sees /repo/helloapp.tpkg shipped in initrd
#   pkg install helloapp
#   ls /data/apps  -- confirms files were extracted
#   pkg list
#   pkg info helloapp
#   pkg remove helloapp
#   pkg list       -- confirms it's gone
#
# Run from the tobyOS directory after `make`.


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
    "-qmp", "tcp:127.0.0.1:4446,server,nowait"
)

$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru -RedirectStandardError $qemuStderr
Write-Host "qemu pid = $($proc.Id), waiting for boot..."
Start-Sleep -Seconds 8

$client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 4446)
$stream = $client.GetStream()
$reader = New-Object System.IO.StreamReader($stream)
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true

function Send-Json($obj) {
    $j = $obj | ConvertTo-Json -Compress -Depth 10
    $writer.WriteLine($j)
    Start-Sleep -Milliseconds 50
}

$null = $reader.ReadLine()
Send-Json @{ execute = 'qmp_capabilities' }
$null = $reader.ReadLine()

function Send-Key($qcode) {
    $payload = @{
        execute   = 'send-key'
        arguments = @{ keys = @( @{ type = 'qcode'; data = $qcode } ) }
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
    Start-Sleep -Milliseconds 250
}

# ---- milestone 16: package manager exercises ----

# (1) Shell should know about the pkg builtin.
Send-Line 'help'

# (2) Repository listing should find /repo/helloapp.tpkg from initrd.
Send-Line 'pkg repo'

# (3) Initially nothing is installed.
Send-Line 'pkg list'

# (4) Install helloapp from the initrd-shipped repo.
Send-Line 'pkg install helloapp'

# (5) Confirm files landed on disk.
Send-Line 'ls /data/apps'
Send-Line 'ls /data/packages'
Send-Line 'cat /data/apps/helloapp.txt'

# (6) Inspect the install record.
Send-Line 'pkg list'
Send-Line 'pkg info helloapp'

# (7) Install again must refuse (idempotency check).
Send-Line 'pkg install helloapp'

# (8) Running the packaged binary via `run` must work.
Send-Line 'run /data/apps/helloapp.elf'
Start-Sleep -Milliseconds 500
Send-Line 'echo packaged app ran'

# (9) Remove the package.
Send-Line 'pkg remove helloapp'

# (10) Confirm files are gone.
Send-Line 'ls /data/apps'
Send-Line 'pkg list'

# (11) Removing again must fail gracefully (no panic).
Send-Line 'pkg remove helloapp'

# (12) Install from an explicit .tpkg path too.
Send-Line 'pkg install /repo/helloapp.tpkg'
Send-Line 'pkg list'
Send-Line 'pkg remove helloapp'

Send-Line 'echo m16-pkg-smoke-test-done'
Start-Sleep -Seconds 2

Send-Json @{ execute = 'quit' }
Start-Sleep -Milliseconds 400
$writer.Close()
$client.Close()
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
Start-Sleep -Milliseconds 300

Write-Host "=== debug.log (tail) ==="
Get-Content $debugLog -ErrorAction SilentlyContinue | Select-Object -Last 120
