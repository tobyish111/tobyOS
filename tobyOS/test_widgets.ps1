# Smoke-test for milestone 11.
#
# Boots tobyOS headless, drives the shell via QMP send-key, launches
# /bin/gui_widgets, types into the focused text input, and asks the
# kernel for diagnostics by spamming Ctrl+C to kill the program. We
# can't take a screenshot from PowerShell easily, but we CAN inspect
# debug.log + serial.log for "[sys_exit]" lines that tell us the GUI
# program ran and exited cleanly under SIGINT (which is what Ctrl+C
# does to a foreground process).

$ErrorActionPreference = 'Stop'

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -ErrorAction SilentlyContinue serial.log, debug.log, qemu.log

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$qemuArgs = @(
    "-cdrom", "tobyOS.iso",
    "-drive", "file=disk.img,format=raw,if=ide,index=0,media=disk",
    "-smp", "4",
    "-serial", "file:serial.log",
    "-debugcon", "file:debug.log",
    "-d", "cpu_reset,guest_errors", "-D", "qemu.log",
    "-no-reboot", "-no-shutdown",
    "-display", "none",
    "-qmp", "tcp:127.0.0.1:4445,server,nowait"
)

$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru -RedirectStandardError "qemu.stderr.log"
Write-Host "qemu pid = $($proc.Id), waiting for boot..."
Start-Sleep -Seconds 8

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

$null = $reader.ReadLine()
Send-Json @{ execute = 'qmp_capabilities' }
$null = $reader.ReadLine()

function Send-Key($qcode) {
    Send-Json @{
        execute   = 'send-key'
        arguments = @{ keys = @( @{ type = 'qcode'; data = $qcode } ) }
    }
    $null = $reader.ReadLine()
}

function Send-Combo($qcodes) {
    Send-Json @{
        execute   = 'send-key'
        arguments = @{ keys = @($qcodes | ForEach-Object { @{ type = 'qcode'; data = $_ } }) }
    }
    $null = $reader.ReadLine()
}

function Send-Text($text) {
    foreach ($ch in $text.ToCharArray()) {
        switch ($ch) {
            ' '  { Send-Key 'spc';                       continue }
            '_'  { Send-Combo @('shift', 'minus');       continue }
            default { Send-Key ([string]$ch).ToLower() }
        }
    }
}

function Send-Line($text) {
    Send-Text $text
    Send-Key 'ret'
    Start-Sleep -Milliseconds 200
}

# Launch the widget demo (foreground -- shell waits).
Send-Line 'gui gui_widgets'
Start-Sleep -Seconds 2

# We're now inside the gui_widgets window. The kernel routes keys to
# the focused widget. The text input doesn't have focus initially
# (we'd need a mouse click to give it focus), but we can still verify
# the key path doesn't crash by typing some characters -- they'll be
# delivered as GUI_EV_KEY events to the focused widget (none yet),
# meaning they get swallowed cleanly. Then Ctrl+C kills the proc.

Send-Text 'hi'
Start-Sleep -Milliseconds 300

# Ctrl+C tears the gui_widgets process down.
Send-Combo @('ctrl', 'c')
Start-Sleep -Seconds 1

# Confirm we're back at the shell prompt.
Send-Line 'echo back-from-widgets'
Start-Sleep -Milliseconds 500

Send-Json @{ execute = 'quit' }
Start-Sleep -Milliseconds 400
$writer.Close()
$client.Close()
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
Start-Sleep -Milliseconds 300

Write-Host ""
Write-Host "=== last 60 lines of serial.log ==="
Get-Content serial.log -Tail 60 -ErrorAction SilentlyContinue
