# test_m26c.ps1 -- M26C USB Hot-Plug validation.
#
# M26C adds:
#   * src/hotplug.c         (kernel-side SPSC event ring, syscall ABI 53)
#   * src/xhci.c            (xhci_detach_slot, xhci_attach_root_port,
#                            xhci_service_port_changes, slot lookups)
#   * src/usb_hub.c         (usb_hub_poll + per-port last_connected
#                            edge tracking for downstream hubs)
#   * src/usb_hid.c         (usb_hid_unbind)
#   * src/usb_msc.c         (usb_msc_unbind)
#   * /bin/usbtest hotplug  (round-trip self-test + drain printout)
#
# Test plan:
#   1. Boot QEMU with qemu-xhci + usb-hub + usb-kbd, plus a QMP socket.
#      The hub is plugged into root port 1; a kbd is on hub-port 1.1
#      (so M26A/B paths still pass at boot).
#   2. Wait for the M26A boot sentinel (proves the boot harness ran
#      AND the synthetic ring round-trip passed via usbtest hotplug).
#   3. Cycle a usb-mouse on/off behind the hub via QMP device_add /
#      device_del. The kernel's usb_hub_poll() (5 Hz) is supposed to
#      detect the C_PORT_CONNECTION transitions and call
#      xhci_attach_via_hub() / xhci_detach_slot(), which in turn each
#      log a one-line marker AND post an ABI_HOT_ATTACH/ABI_HOT_DETACH
#      event into the kernel ring.
#   4. Cycle a usb-mouse on/off on a *root* port too -- exercises
#      the xhci_poll() Port Status Change path + service-deferred
#      attach/detach that DOESN'T go through usb_hub_poll().
#   5. Stop QEMU, grep serial.m26c.log for the expected attach/detach
#      lines, verify no panics, verify nothing regressed from M26A/B.
#
# Exit 0 => PASS, exit 1 => FAIL (with the missing patterns listed).

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m26c"
$serial   = "serial.$logTag.log"
$debug    = "debug.$logTag.log"
$qemuLog  = "qemu.$logTag.log"
$qmpPort  = 4455
$bootSentinel = '\[boot\] M26A: userland .* PASS .* of \d+'
$bootTimeoutSec = 60

if (-not (Test-Path $iso))      { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk))     { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M26C: USB Hot-Plug ==============" -ForegroundColor Cyan

$qemuArgs = @(
    "-cdrom", $iso,
    "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
    "-smp", "4",
    "-netdev", "user,id=net0",
    "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
    # USB topology: xhci + 8-port hub on root port 1, kbd on hub-port
    # 1.1 so boot enumeration has something to walk + assert against.
    # Leave hub-ports 1.5 / 1.6 free for QMP device_add and root port 4
    # also free (we hot-plug a mouse straight onto the root hub there).
    "-device", "qemu-xhci,id=usb0",
    "-device", "usb-hub,bus=usb0.0,port=1,id=hub1",
    "-device", "usb-kbd,bus=usb0.0,port=1.1",
    "-serial", "file:$serial",
    "-debugcon", "file:$debug",
    "-d", "cpu_reset,guest_errors", "-D", $qemuLog,
    # QMP socket -- "nowait" so QEMU still boots even if we don't
    # connect immediately. server=on lets us connect from the script
    # via TCP from PowerShell once the boot sentinel has fired.
    "-qmp", "tcp:127.0.0.1:${qmpPort},server=on,wait=off",
    "-no-reboot", "-display", "none"
)
$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
Write-Host "[m26c] qemu pid = $($proc.Id), QMP port = $qmpPort"

# ---- 1. Wait for boot sentinel (M26A passes) ----
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

Write-Host "[m26c] boot sentinel fired -- driving hot-plug cycles via QMP" -ForegroundColor Cyan

# ---- 2. QMP helpers ----
function Open-QMP {
    param([int]$port)
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect("127.0.0.1", $port)
    $stream = $client.GetStream()
    # Read greeting (single JSON line ending in \r\n).
    $buf = New-Object byte[] 4096
    $n = $stream.Read($buf, 0, $buf.Length)
    $greeting = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
    if ($greeting -notmatch 'QMP') {
        throw "QMP greeting unexpected: $greeting"
    }
    # Hand-shake: enable command mode.
    $cmd = '{"execute":"qmp_capabilities"}' + "`n"
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

function Close-QMP {
    param($conn)
    if ($conn -and $conn.client) { $conn.client.Close() }
}

# Connect.
$qmp = $null
try {
    $qmp = Open-QMP -port $qmpPort
} catch {
    Write-Host "FAIL: could not connect to QMP on 127.0.0.1:$qmpPort -- $_" -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

# ---- 3. Cycle a usb-mouse behind the hub a few times ----
$cycles = 3
for ($i = 1; $i -le $cycles; $i++) {
    $id  = "hubmouse$i"
    Write-Host "[m26c] cycle ${i}: hub-port 1.5 device_add $id" -ForegroundColor Yellow
    $r = Send-QMP -conn $qmp -json "{`"execute`":`"device_add`",`"arguments`":{`"driver`":`"usb-mouse`",`"id`":`"$id`",`"bus`":`"usb0.0`",`"port`":`"1.5`"}}"
    Write-Host "  -> $r"
    # usb_hub_poll runs at 5 Hz; give it ~600ms to see the change.
    Start-Sleep -Milliseconds 700
    Write-Host "[m26c] cycle ${i}: hub-port 1.5 device_del $id" -ForegroundColor Yellow
    $r = Send-QMP -conn $qmp -json "{`"execute`":`"device_del`",`"arguments`":{`"id`":`"$id`"}}"
    Write-Host "  -> $r"
    Start-Sleep -Milliseconds 700
}

# ---- 4. Cycle a usb-mouse on a free root-hub port ----
# Root port 4 is unused (hub is on root port 1). On QEMU's xhci this
# fires a real Port Status Change TRB -- exactly the event-ring path
# we wire up via xhci_service_port_changes().
for ($i = 1; $i -le $cycles; $i++) {
    $id  = "rootmouse$i"
    Write-Host "[m26c] root-port cycle ${i}: device_add $id (port=4)" -ForegroundColor Yellow
    $r = Send-QMP -conn $qmp -json "{`"execute`":`"device_add`",`"arguments`":{`"driver`":`"usb-mouse`",`"id`":`"$id`",`"bus`":`"usb0.0`",`"port`":`"4`"}}"
    Write-Host "  -> $r"
    Start-Sleep -Milliseconds 700
    Write-Host "[m26c] root-port cycle ${i}: device_del $id" -ForegroundColor Yellow
    $r = Send-QMP -conn $qmp -json "{`"execute`":`"device_del`",`"arguments`":{`"id`":`"$id`"}}"
    Write-Host "  -> $r"
    Start-Sleep -Milliseconds 700
}

Close-QMP -conn $qmp

# Give the kernel one more service tick + flush serial.
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
    # --- M26C synthetic ring round-trip from boot harness ---
    'PASS: usbtest hotplug ring: ring=64 capacity=64 posted=\d+ drained=\d+ dropped=\d+',

    # --- hub poll detected at least one ATTACH cycle ---
    '\[usb_hub\] hot-attach hub-slot \d+ port 5',
    # --- hub poll detected at least one DETACH cycle ---
    '\[usb_hub\] hot-detach hub-slot \d+ port 5',
    # --- detach actually freed the slot through xhci. The device path
    #     is "usb1-<root_port>.<hub_port>"; root_port is whichever root
    #     port QEMU assigned the hub to (we use \d+ since QEMU's xhci
    #     places usb-hub on its own choice of free port). ---
    '\[xhci\] detach slot \d+ \(usb1-\d+\.5 ',

    # --- root-port hot-plug went through the deferred service path.
    #     QEMU's qemu-xhci splits its 8 ports between USB-3 (1-4) and
    #     USB-2 (5-8). A FS usb-mouse asked for QMP `port=4` actually
    #     lands on the USB-2 half (root port 8). Match \d+ to be
    #     QEMU-revision-tolerant. ---
    '\[xhci\] hot-attach root port \d+ \(PORTSC=0x[0-9a-fA-F]+\)',
    '\[xhci\] hot-detach root port \d+ \(slot \d+\)',

    # --- M26B invariants (must not regress) ---
    '\[usb_hub\] slot \d+: HUB nports=\d+',
    '\[PASS\] usb_hub:',
    # --- M26A invariants ---
    '\[boot\] M26A: peripheral inventory \+ self-tests',
    '\[PASS\] xhci:',
    '\[PASS\] hotplug:',
    '\[boot\] M26A: /bin/usbtest .*PASS\)',
    '\[boot\] M26A: userland (\d+) PASS / 0 FAIL / 0 missing of \1'
)
$missing = @()
foreach ($pat in $mustHave) {
    if ($txt -notmatch $pat) { $missing += $pat }
}

# === forbidden tokens ===
$forbidden = @('panic', 'page fault', 'KERNEL OOPS',
               '\[FAIL\] usb_hub', '\[FAIL\] xhci', '\[FAIL\] pci', '\[FAIL\] hotplug')
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M26C PASS -- hot-plug cycles detected by both root + hub paths, no leaks." -ForegroundColor Green
    Write-Host ""
    Write-Host "Hot-plug attach/detach lines from ${serial}:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '(hot-attach|hot-detach|detach slot)' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "Synthetic ring round-trip:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern 'usbtest hotplug ring' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "Final harness summary:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '\[boot\] M26A: userland .* PASS' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    exit 0
} else {
    Write-Host "M26C FAIL" -ForegroundColor Red
    if ($missing.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($panics.Count -gt 0) {
        Write-Host "Forbidden tokens present (kernel was unhappy):" -ForegroundColor Red
        $panics | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 150 lines of ${serial} ===" -ForegroundColor Yellow
    Get-Content $serial -Tail 150
    exit 1
}
