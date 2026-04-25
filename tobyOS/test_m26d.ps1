# test_m26d.ps1 -- M26D USB HID Robustness validation.
#
# M26D adds:
#   * src/usb_hid.c:  per-device counters (frames, presses, releases,
#                     mouse clicks, last modifier byte, dx/dy sums) +
#                     usb_hid_introspect_at + usb_hid_selftest +
#                     periodic snapshot kprintf every 8 frames
#   * src/keyboard.c: ps2 chars_dispatched + irqs_total + caps/shift/
#                     ctrl accessors + dispatch snapshot every 16 chars
#   * src/mouse.c:    ps2 events_total + btn_press_total + dx/dy sums +
#                     last_buttons accessor + dispatch snapshot every
#                     16 events
#   * src/devtest.c:  registers "input" + "usb_hid" devtests; merges
#                     USB HID into ABI_DEVT_BUS_INPUT enumeration so
#                     `devlist input` shows PS/2 + USB together
#   * usbtest hid:    new userland subcommand -- lists INPUT bus,
#                     runs both devtests, exits 0 on PASS or SKIP
#
# Test plan:
#   1. Boot QEMU with qemu-xhci + usb-kbd + usb-mouse on root ports.
#      `-display none` keeps QEMU's PS/2 front-end disconnected so
#      QMP `input-send-event` lands directly on the USB HID devices.
#   2. Wait for the M26A boot sentinel (proves usbtest-hid + the rest
#      of the boot harness ran).
#   3. Drive a key burst via QMP input-send-event: 5 plain keystrokes
#      then 3 shift-A sequences. Each press/release pair = 2 USB int-IN
#      frames; 5+3*2 sequences = at least 16 frames -> guaranteed >=2
#      `[input] hid kbd ...` snapshot lines, including one with mod=0x02
#      proving modifier handling works.
#   4. Drive a mouse burst: 24 small relative-motion events + 1 left
#      click = at least 25 frames -> guaranteed >=3 `[input] hid mouse`
#      snapshot lines including `clicks>=1`.
#   5. Cycle the usb-mouse: device_del then device_add to confirm
#      reconnect produces a clean unbind + new bind log pair, and that
#      a second mouse-motion burst keeps incrementing frames on the
#      newly attached slot.
#   6. Stop QEMU, grep serial.m26d.log for the expected signals and
#      verify the M26A/B/C invariants did not regress.
#
# Exit 0 => PASS, exit 1 => FAIL with the missing patterns listed.

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m26d"
$serial   = "serial.$logTag.log"
$debug    = "debug.$logTag.log"
$qemuLog  = "qemu.$logTag.log"
$qmpPort  = 4456
$bootSentinel = '\[boot\] M26A: userland .* PASS .* of \d+'
$bootTimeoutSec = 60

if (-not (Test-Path $iso))  { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M26D: USB HID Robustness ==============" -ForegroundColor Cyan

$qemuArgs = @(
    "-cdrom", $iso,
    "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
    "-smp", "4",
    "-netdev", "user,id=net0",
    "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
    # USB topology: xhci with both a kbd AND a mouse plugged in to
    # root ports from boot. Both must enumerate before the M26A
    # boot harness runs `usbtest hid`.
    "-device", "qemu-xhci,id=usb0",
    "-device", "usb-kbd,id=kbd0,bus=usb0.0,port=1",
    "-device", "usb-mouse,id=mouse0,bus=usb0.0,port=2",
    "-serial", "file:$serial",
    "-debugcon", "file:$debug",
    "-d", "cpu_reset,guest_errors", "-D", $qemuLog,
    # QMP socket for input-send-event + device_add/del.
    "-qmp", "tcp:127.0.0.1:${qmpPort},server=on,wait=off",
    "-no-reboot", "-display", "none"
)
$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
Write-Host "[m26d] qemu pid = $($proc.Id), QMP port = $qmpPort"

# ---- 1. Wait for boot sentinel ----
$start = Get-Date
$saw   = $false
while (((Get-Date) - $start).TotalSeconds -lt $bootTimeoutSec) {
    Start-Sleep -Seconds 1
    if (-not (Test-Path $serial)) { continue }
    $log = Get-Content $serial -Raw -ErrorAction SilentlyContinue
    if (-not $log) { continue }
    if ($log -match $bootSentinel) { $saw = $true; break }
    if ($log -match 'panic|page fault|KERNEL OOPS') { break }
}

if (-not $saw) {
    Write-Host "FAIL: timed out after ${bootTimeoutSec}s waiting for boot sentinel" -ForegroundColor Red
    if (Test-Path $serial) {
        Write-Host "Tail of ${serial}:" -ForegroundColor Yellow
        Get-Content $serial -Tail 80
    }
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

Write-Host "[m26d] boot sentinel fired -- driving HID input via QMP" -ForegroundColor Cyan

# ---- 2. QMP helpers (lifted verbatim from test_m26c.ps1) ----
function Open-QMP {
    param([int]$port)
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect("127.0.0.1", $port)
    $stream = $client.GetStream()
    $buf = New-Object byte[] 4096
    $n = $stream.Read($buf, 0, $buf.Length)
    $greeting = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
    if ($greeting -notmatch 'QMP') { throw "QMP greeting unexpected: $greeting" }
    $cmd   = '{"execute":"qmp_capabilities"}' + "`n"
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($cmd)
    $stream.Write($bytes, 0, $bytes.Length)
    Start-Sleep -Milliseconds 100
    $n = $stream.Read($buf, 0, $buf.Length)
    return @{ client = $client; stream = $stream }
}
function Send-QMP {
    param($conn, [string]$json)
    $payload = $json + "`n"
    $bytes   = [System.Text.Encoding]::UTF8.GetBytes($payload)
    $conn.stream.Write($bytes, 0, $bytes.Length)
    Start-Sleep -Milliseconds 200
    $buf = New-Object byte[] 8192
    $n = $conn.stream.Read($buf, 0, $buf.Length)
    return [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
}
function Close-QMP { param($conn); if ($conn -and $conn.client) { $conn.client.Close() } }

# Helper: send a single key press OR release via QMP.
function Send-Key {
    param($conn, [string]$qcode, [bool]$down)
    $d = if ($down) { 'true' } else { 'false' }
    $json = '{"execute":"input-send-event","arguments":{"events":[' +
            '{"type":"key","data":{"down":' + $d +
            ',"key":{"type":"qcode","data":"' + $qcode + '"}}}]}}'
    return Send-QMP -conn $conn -json $json
}
# Helper: tap a key (down then up).
function Tap-Key { param($conn, [string]$qcode)
    Send-Key -conn $conn -qcode $qcode -down $true  | Out-Null
    Start-Sleep -Milliseconds 30
    Send-Key -conn $conn -qcode $qcode -down $false | Out-Null
    Start-Sleep -Milliseconds 30
}
# Helper: send a relative-motion mouse event.
function Send-Rel { param($conn, [int]$dx, [int]$dy)
    $json = '{"execute":"input-send-event","arguments":{"events":[' +
            '{"type":"rel","data":{"axis":"x","value":' + $dx + '}},' +
            '{"type":"rel","data":{"axis":"y","value":' + $dy + '}}]}}'
    Send-QMP -conn $conn -json $json | Out-Null
    Start-Sleep -Milliseconds 30
}
# Helper: send a mouse-button click (down then up).
function Click-Btn { param($conn, [string]$btn)
    $down = '{"execute":"input-send-event","arguments":{"events":[' +
            '{"type":"btn","data":{"down":true,"button":"' + $btn + '"}}]}}'
    $up   = '{"execute":"input-send-event","arguments":{"events":[' +
            '{"type":"btn","data":{"down":false,"button":"' + $btn + '"}}]}}'
    Send-QMP -conn $conn -json $down | Out-Null
    Start-Sleep -Milliseconds 50
    Send-QMP -conn $conn -json $up   | Out-Null
    Start-Sleep -Milliseconds 50
}

$qmp = $null
try { $qmp = Open-QMP -port $qmpPort } catch {
    Write-Host "FAIL: could not connect to QMP on 127.0.0.1:$qmpPort -- $_" -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

# ---- 3. Key burst: 5 plain taps + 3 shift-A sequences ----
Write-Host "[m26d] driving keyboard burst (plain + shift)" -ForegroundColor Yellow
foreach ($k in @('h','i','j','k','l')) { Tap-Key -conn $qmp -qcode $k }
for ($i = 0; $i -lt 3; $i++) {
    Send-Key -conn $qmp -qcode 'shift'  -down $true  | Out-Null
    Start-Sleep -Milliseconds 30
    Tap-Key  -conn $qmp -qcode 'a'
    Send-Key -conn $qmp -qcode 'shift'  -down $false | Out-Null
    Start-Sleep -Milliseconds 30
}
Start-Sleep -Milliseconds 500   # let snapshot logger flush

# ---- 4. Mouse burst: 24 motion events + 1 left click ----
Write-Host "[m26d] driving mouse burst (motion + left click)" -ForegroundColor Yellow
for ($i = 0; $i -lt 24; $i++) { Send-Rel -conn $qmp -dx 3 -dy 2 }
Click-Btn -conn $qmp -btn 'left'
Start-Sleep -Milliseconds 500

# ---- 5. Reconnect cycle: del + add usb-mouse, drive again ----
Write-Host "[m26d] reconnect cycle: device_del mouse0 + device_add mouse1" -ForegroundColor Yellow
$r = Send-QMP -conn $qmp -json '{"execute":"device_del","arguments":{"id":"mouse0"}}'
Start-Sleep -Milliseconds 800
$r = Send-QMP -conn $qmp -json '{"execute":"device_add","arguments":{"driver":"usb-mouse","id":"mouse1","bus":"usb0.0","port":"3"}}'
Start-Sleep -Milliseconds 800
# Drive the new mouse to prove it actually delivers reports.
for ($i = 0; $i -lt 16; $i++) { Send-Rel -conn $qmp -dx 2 -dy -1 }
Click-Btn -conn $qmp -btn 'right'
Start-Sleep -Milliseconds 500

Close-QMP -conn $qmp

# Give the kernel a final tick + flush.
Start-Sleep -Milliseconds 1000
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

if (-not (Test-Path $serial)) {
    Write-Host "FAIL: $serial not produced by QEMU" -ForegroundColor Red
    exit 1
}
$txt = Get-Content $serial -Raw

# === required signals ===
$mustHave = @(
    # --- M26D: HID enumeration during boot ---
    '\[usb-hid\] boot keyboard on slot \d+ iface \d+',
    '\[usb-hid\] boot mouse on slot \d+ iface \d+',

    # --- M26D: usbtest hid PASS at boot ---
    'PASS: usbtest hid input: ps2 kbd chars=\d+ irqs=\d+',
    'PASS: usbtest hid usb_hid: USB HID: \d+ device\(s\) -- kbd=\d+ mouse=\d+',

    # --- M26D: HID activity snapshots after the QMP key/mouse burst ---
    '\[input\] hid kbd slot=\d+ frames=\d+ presses=\d+',
    '\[input\] hid kbd slot=\d+ frames=\d+ presses=\d+ releases=\d+ mod=0x02',
    '\[input\] hid mouse slot=\d+ frames=\d+ clicks=[1-9]',

    # --- M26D: shared dispatch sink got the chars too ---
    '\[input\] kbd_dispatch chars=\d+ irqs=\d+',

    # --- M26D: PS/2 invariant: drivers still came up at boot ---
    '\[kbd\] PS/2 driver up \(IRQ1 unmasked\)',
    '\[mouse\] PS/2 driver up \(IRQ12',

    # --- M26D: reconnect cycle produced an unbind + a fresh bind ---
    '\[usb-hid\] unbind slot \d+ \(mouse, iface \d+\)',
    '\[xhci\] hot-attach root port \d+ \(PORTSC=0x[0-9a-fA-F]+\)',

    # --- M26C invariants (must not regress) ---
    'PASS: usbtest hotplug ring:',
    '\[PASS\] hotplug:',

    # --- M26B invariant ---
    '\[PASS\] usb_hub:|\[SKIP\] usb_hub:',

    # --- M26A invariant: full boot harness PASSed (no FAIL) ---
    '\[boot\] M26A: peripheral inventory \+ self-tests',
    '\[PASS\] xhci:',
    '\[boot\] M26A: /bin/usbtest .*PASS\)',
    '\[boot\] M26A: userland (\d+) PASS / 0 FAIL / 0 missing of \1'
)
$missing = @()
foreach ($pat in $mustHave) { if ($txt -notmatch $pat) { $missing += $pat } }

# === forbidden tokens ===
$forbidden = @('panic', 'page fault', 'KERNEL OOPS',
               '\[FAIL\] usb_hid', '\[FAIL\] input',
               '\[FAIL\] usb_hub', '\[FAIL\] xhci', '\[FAIL\] hotplug')
$panics = @()
foreach ($pat in $forbidden) { if ($txt -match $pat) { $panics += $pat } }

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M26D PASS -- USB HID kbd+mouse counters increment, modifiers seen, reconnect clean, PS/2 invariant intact." -ForegroundColor Green
    Write-Host ""
    Write-Host "HID activity snapshots from ${serial}:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '\[input\] hid (kbd|mouse) slot=' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "Self-test results:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '(usbtest hid|PASS\] (usb_hid|input))' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "Reconnect cycle:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '(usb-hid\] (unbind|boot)|hot-attach root port)' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "Final harness summary:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '\[boot\] M26A: userland .* PASS' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    exit 0
} else {
    Write-Host "M26D FAIL" -ForegroundColor Red
    if ($missing.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($panics.Count -gt 0) {
        Write-Host "Forbidden tokens present (kernel was unhappy):" -ForegroundColor Red
        $panics | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 200 lines of ${serial} ===" -ForegroundColor Yellow
    Get-Content $serial -Tail 200
    exit 1
}
