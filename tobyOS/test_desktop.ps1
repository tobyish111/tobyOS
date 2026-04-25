# Smoke-test for milestone 12.
#
# Boots tobyOS headless, drives the shell via QMP send-key, enters the
# desktop environment, simulates a mouse click on the [Apps] start
# button + a launcher entry to spawn /bin/gui_about, then asks the
# kernel for diagnostics. Inspects serial.log for "[gui] entering
# graphical mode" + "[gui] launched" + "[sys_exit]" to confirm the
# pipeline ran end-to-end without crashing.

$ErrorActionPreference = 'Stop'

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -ErrorAction SilentlyContinue serial.log, debug.log, qemu.log

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
# Note: we still rely on the PS/2 mouse for input -- the kernel only
# wires up that driver. QMP -> PS/2 packet conversion clamps deltas to
# 8-bit signed (the PS/2 wire format), so we send many small nudges
# instead of one big jump. See Mouse-MoveTo below.
$qemuArgs = @(
    "-cdrom", "tobyOS.iso",
    "-drive", "file=disk.img,format=raw,if=ide,index=0,media=disk",
    "-smp", "4",
    "-serial", "file:serial.log",
    "-debugcon", "file:debug.log",
    "-d", "cpu_reset,guest_errors", "-D", "qemu.log",
    "-no-reboot", "-no-shutdown",
    "-display", "none",
    "-vnc", ":99",
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
    Start-Sleep -Milliseconds 60
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
            ' '  { Send-Key 'spc'; continue }
            default { Send-Key ([string]$ch).ToLower() }
        }
    }
}

function Send-Line($text) {
    Send-Text $text
    Send-Key 'ret'
    Start-Sleep -Milliseconds 250
}

# Mouse helpers: PS/2 packets use an 8-bit signed delta, so we send
# many small deltas. Home the cursor at (0,0) by spamming negative
# nudges, then walk it to the target. Default framebuffer is 1280x800.
function Mouse-Nudge([int]$dx, [int]$dy) {
    Send-Json @{
        execute = 'input-send-event'
        arguments = @{ events = @(
            @{ type = 'rel'; data = @{ axis = 'x'; value = [int]$dx } },
            @{ type = 'rel'; data = @{ axis = 'y'; value = [int]$dy } }
        ) }
    }
    $resp = $reader.ReadLine()
    if ($resp -and $resp.Contains('error')) { Write-Host "QMP err: $resp" }
}
function Mouse-Home() {
    for ($i = 0; $i -lt 20; $i++) { Mouse-Nudge -120 -120 }
    Start-Sleep -Milliseconds 80
}
function Mouse-MoveTo($x, $y) {
    Mouse-Home
    while ($x -gt 0 -or $y -gt 0) {
        $sx = if ($x -gt 100) { 100 } else { $x }
        $sy = if ($y -gt 100) { 100 } else { $y }
        Mouse-Nudge $sx $sy
        $x -= $sx; $y -= $sy
        Start-Sleep -Milliseconds 5
    }
    Start-Sleep -Milliseconds 80
}

function Mouse-Click() {
    Send-Json @{
        execute = 'input-send-event'
        arguments = @{ events = @( @{ type = 'btn'; data = @{ button = 'left'; down = $true } } ) }
    }
    $null = $reader.ReadLine()
    Start-Sleep -Milliseconds 80
    Send-Json @{
        execute = 'input-send-event'
        arguments = @{ events = @( @{ type = 'btn'; data = @{ button = 'left'; down = $false } } ) }
    }
    $null = $reader.ReadLine()
    Start-Sleep -Milliseconds 80
}

# 1) Confirm the new banner / about message.
Send-Line 'about'
Start-Sleep -Milliseconds 400

# 2) Enter desktop mode.
Send-Line 'desktop'
Start-Sleep -Seconds 1

# 3) Click the [Apps] start button. Framebuffer is 1280x800, taskbar
#    y = 800 - 24 = 776; start button rect (2,778)-(58,798), centre
#    at (30, 788).
Mouse-MoveTo 30 788
Mouse-Click
Start-Sleep -Milliseconds 500

# 4) Click "About system" (launcher entry index 3). Menu height with
#    5 entries = 5*22 + 8 = 118 -> menu y = 776 - 118 = 658.
#    Item 3 centre y = 658 + 4 + 3*22 + 7 = 735.
Mouse-MoveTo 90 735
Mouse-Click
Start-Sleep -Seconds 2

# 5) Click the close-X on gui_about's title bar. Default window spawn
#    is at (60, 40) with client 360x220 -> outer 362x238. The close
#    button rect is the top-right 14x14 inside the title bar:
#       bx = 60 + 362 - 14 - 2 = 406, by = 40 + (18-14)/2 = 42
#    Centre: (413, 49). Clicking it sends SIGINT to gui_about's pid.
Mouse-MoveTo 413 49
Mouse-Click
Start-Sleep -Seconds 1

# 6) Now open the menu again and click "Exit Desktop" (index 4) so the
#    compositor releases the framebuffer back to the text shell.
Mouse-MoveTo 30 788
Mouse-Click
Start-Sleep -Milliseconds 500
# Item 4 centre y = 658 + 4 + 4*22 + 7 = 757.
Mouse-MoveTo 90 757
Mouse-Click
Start-Sleep -Seconds 1

Send-Line 'echo back-from-desktop'
Start-Sleep -Milliseconds 500

Send-Json @{ execute = 'quit' }
Start-Sleep -Milliseconds 400
$writer.Close()
$client.Close()
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
Start-Sleep -Milliseconds 300

Write-Host ""
Write-Host "=== grep: milestone 12 markers in serial.log ==="
$log = Get-Content serial.log -ErrorAction SilentlyContinue
$patterns = @(
    'milestone 12',
    'desktop: entered',
    '\[gui\] entering graphical mode',
    '\[gui\] launched /bin/gui_about',
    '\[proc\] pid=\d+ ./bin/gui_about. exit code=130',  # SIGINT exit (128+2)
    '\[gui\] returning to text mode',
    'backfromdesktop'
)
foreach ($p in $patterns) {
    $hit = $log | Select-String -Pattern $p
    if ($hit) {
        Write-Host "  OK   $p"
    } else {
        Write-Host "  MISS $p"
    }
}

Write-Host ""
Write-Host "=== last 80 lines of serial.log ==="
Get-Content serial.log -Tail 80 -ErrorAction SilentlyContinue
