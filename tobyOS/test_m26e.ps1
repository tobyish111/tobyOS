# test_m26e.ps1 -- M26E USB Mass Storage Robustness validation.
#
# M26E adds:
#   * include/tobyos/blk.h, src/blk.c
#       - new `bool gone` flag on struct blk_dev
#       - blk_mark_gone(disk) -- recursively flags the disk + its
#         partitions so blk_read/blk_write short-circuit to -EIO.
#   * include/tobyos/vfs.h, src/vfs.c
#       - vfs_unmount(mount_point) drops a mount and invokes the
#         filesystem's optional umount op.
#       - vfs_iter_mounts(cb, cookie) lets the MSC driver discover
#         mount points sitting on a yanked disk.
#   * include/tobyos/fat32.h, src/fat32.c
#       - fat32_umount frees scratch buffers + flushes the FAT cache
#         best-effort (skipped if the device is already gone).
#       - fat32_blkdev_of(mnt) maps an opaque mount-data pointer back
#         to the underlying blk_dev (used by introspection).
#   * include/tobyos/usb_msc.h, src/usb_msc.c
#       - per-slot stats (reads_ok/eio, writes_ok/eio, bytes, safe vs
#         unsafe removal counters).
#       - usb_msc_unbind: detect any FAT32 mount sitting on the disk,
#         log a loud warning, mark the disk gone, force-unmount.
#       - usb_msc_introspect_at + usb_msc_selftest -- non-destructive,
#         registered via devtest as "usb_msc".
#   * src/devtest.c
#       - devt_emit_blk now includes "gone" + "mount=<path>" tokens
#         in the BLK record extras and adds ABI_DEVT_ACTIVE for
#         currently-mounted disks.
#       - registers "usb_msc" selftest.
#   * src/kernel.c
#       - boot-time /usb smoke test now does an explicit
#         vfs_unmount("/usb") + remount round trip.
#   * programs/usbtest/main.c
#       - new `usbtest storage` subcommand with FAT32 RW round-trip.
#
# Test plan:
#   1. Boot QEMU with qemu-xhci + usb-storage at root-port (id=stick0)
#      backed by build/usbstick.img (the same FAT32 stick image M23C
#      uses). The kernel auto-mounts it at /usb and the boot smoke
#      test exercises mount -> RW -> unmount -> remount -> RW.
#   2. Wait for the M26A boot sentinel (proves the harness ran).
#   3. Verify boot-time M26E signals:
#        - "[usb-msc-test]   unmounted /usb cleanly"
#        - "[usb-msc-test]   remounted /usb via 'usb0'"
#        - "[usb-msc-test]   remount RW round-trip PASS"
#        - "[usb-msc-test] selftest PASS"
#        - "PASS: usbtest storage:" (from /bin/usbtest storage)
#   4. Trigger an UNSAFE removal: QMP device_del on stick0 while
#      /usb is still mounted (boot only unmounts/remounts; at the
#      end of boot smoke /usb is mounted again).
#   5. Verify:
#        - "[usb-msc] WARN: 'usb0' yanked while \d+ mount(s) active"
#        - "[usb-msc]   unsafe-removal: forcing umount of '/usb'"
#        - "[blk] 'usb0' marked gone"
#        - "[vfs] unmounted '/usb'"
#        - no panic / page fault.
#   6. Re-attach via device_add, give the kernel a tick to re-enumerate
#      and confirm the new disk binds cleanly without crashing.
#
# Exit 0 => PASS, exit 1 => FAIL with a list of missing/forbidden patterns.

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$stick    = "build/usbstick.img"
$logTag   = "m26e"
$serial   = "serial.$logTag.log"
$debug    = "debug.$logTag.log"
$qemuLog  = "qemu.$logTag.log"
$qmpPort  = 4456
$bootSentinel  = '\[boot\] M26A: userland .* PASS .* of \d+'
$bootTimeoutSec = 90

if (-not (Test-Path $iso))   { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk))  { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $stick)) { Write-Host "$stick missing -- run 'make $stick' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M26E: USB Mass Storage Robustness ==============" -ForegroundColor Cyan

$qemuArgs = @(
    "-cdrom", $iso,
    "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
    "-drive", "if=none,id=usbstick,format=raw,file=$stick",
    "-smp", "4",
    "-netdev", "user,id=net0",
    "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
    # USB topology: xhci + usb-storage at root port. We deliberately
    # use an explicit `id=stick0` so QMP device_del has a target.
    "-device", "qemu-xhci,id=usb0",
    "-device", "usb-storage,bus=usb0.0,drive=usbstick,id=stick0",
    "-serial", "file:$serial",
    "-debugcon", "file:$debug",
    "-d", "cpu_reset,guest_errors", "-D", $qemuLog,
    "-qmp", "tcp:127.0.0.1:${qmpPort},server=on,wait=off",
    "-no-reboot", "-display", "none"
)
$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
Write-Host "[m26e] qemu pid = $($proc.Id), QMP port = $qmpPort"

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

Write-Host "[m26e] boot sentinel fired -- driving unsafe removal via QMP" -ForegroundColor Cyan

# ---- 2. QMP helpers (mirror of test_m26c.ps1) ----
function Open-QMP {
    param([int]$port)
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect("127.0.0.1", $port)
    $stream = $client.GetStream()
    $buf = New-Object byte[] 4096
    $n = $stream.Read($buf, 0, $buf.Length)
    $greeting = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
    if ($greeting -notmatch 'QMP') {
        throw "QMP greeting unexpected: $greeting"
    }
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
    Start-Sleep -Milliseconds 250
    $buf = New-Object byte[] 8192
    $n = $conn.stream.Read($buf, 0, $buf.Length)
    return [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
}

function Close-QMP {
    param($conn)
    if ($conn -and $conn.client) { $conn.client.Close() }
}

$qmp = $null
try {
    $qmp = Open-QMP -port $qmpPort
} catch {
    Write-Host "FAIL: could not connect to QMP on 127.0.0.1:$qmpPort -- $_" -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

# ---- 3. UNSAFE removal: device_del while /usb is mounted ----
# After boot the smoke test re-mounts /usb (remount round-trip), so
# the mount IS live when we yank. xhci_service_port_changes runs at
# every kernel idle tick so the detach is seen within a few ms.
Write-Host "[m26e] device_del stick0 (UNSAFE removal -- /usb still mounted)" -ForegroundColor Yellow
$r = Send-QMP -conn $qmp -json '{"execute":"device_del","arguments":{"id":"stick0"}}'
Write-Host "  -> $r"
Start-Sleep -Milliseconds 1500

# ---- 4. Re-attach the same backing file under a new id ----
Write-Host "[m26e] device_add stick1 (re-attach same drive)" -ForegroundColor Yellow
$r = Send-QMP -conn $qmp -json '{"execute":"device_add","arguments":{"driver":"usb-storage","id":"stick1","bus":"usb0.0","drive":"usbstick"}}'
Write-Host "  -> $r"
Start-Sleep -Milliseconds 1500

Close-QMP -conn $qmp

# Give the kernel one more service tick + flush serial.
Start-Sleep -Milliseconds 1500
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

if (-not (Test-Path $serial)) {
    Write-Host "FAIL: $serial not produced by QEMU" -ForegroundColor Red
    exit 1
}
$txt = Get-Content $serial -Raw

# === required signals ===
$mustHave = @(
    # --- M26E boot smoke: explicit unmount + remount + RW ---
    '\[usb-msc-test\]   unmounted /usb cleanly',
    "\[usb-msc-test\]   remounted /usb via 'usb0'",
    '\[usb-msc-test\]   remount RW round-trip PASS \(\d+ bytes\)',

    # --- M26E userland: usbtest storage subcommand ---
    'PASS: usbtest storage:',

    # --- M26E unsafe removal: WARN + forced umount + gone marker ---
    "\[usb-msc\] WARN: 'usb0' yanked while \d+ mount\(s\) active",
    "\[usb-msc\]   unsafe-removal: forcing umount of '/usb'",
    "\[blk\] 'usb0' marked gone",
    "\[vfs\] unmounted '/usb'",

    # --- regressions guarded ---
    '\[boot\] M26A: peripheral inventory \+ self-tests',
    '\[PASS\] xhci:',
    '\[boot\] M26A: userland (\d+) PASS / 0 FAIL / 0 missing of \1'
)
$missing = @()
foreach ($pat in $mustHave) {
    if ($txt -notmatch $pat) { $missing += $pat }
}

# === forbidden tokens ===
$forbidden = @('panic', 'page fault', 'KERNEL OOPS',
               '\[FAIL\] usb_msc:', '\[FAIL\] xhci:', '\[FAIL\] hotplug:')
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M26E PASS -- mount/unmount cycle clean, unsafe removal handled with WARN + forced umount." -ForegroundColor Green
    Write-Host ""
    Write-Host "Boot-time mount/unmount/remount evidence:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern 'usb-msc-test\].*(?:unmount|remount|round-trip)' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "Unsafe-removal trail:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '(usb-msc.*WARN|unsafe-removal|marked gone|\[vfs\] unmounted)' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "usbtest storage result:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '(usbtest storage|usb_msc:)' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    exit 0
} else {
    Write-Host "M26E FAIL" -ForegroundColor Red
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
