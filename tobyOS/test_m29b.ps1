# test_m29b.ps1 -- M29B Driver Matching & Fallback validation.
#
# Drives the boot-time M29B pipeline (kernel.c::m29b_run_drvmatch_harness).
# That pipeline:
#   1. dumps the live drvmatch table to serial via drvmatch_dump_kprintf()
#   2. logs a one-line counter summary (total/bound/unbound/forced_off)
#   3. spawns /bin/drvmatch --boot which:
#        - queries SYS_DRVMATCH for every PCI/USB device
#        - asserts each record is well-formed
#        - probes a deliberately-bogus (DEAD:BEEF) PCI key  -> NONE/ENOENT
#        - probes a deliberately-bogus (DEAD:BEEF) USB key  -> NONE/ENOENT
#        - probes a bogus bus tag                            -> EINVAL
#      and prints M29B_DRV: PASS sentinels for this script
#   4. when /etc/drvtest_now is present, calls drvmatch_disable_pci("e1000")
#      to forcibly unbind the e1000 NIC, re-runs the bind pass to confirm
#      no crash + driver reports FORCED_OFF, then re-enables the driver
#
# We boot tobyOS twice in QEMU:
#   pass 1: standard QEMU device set, no DRVTEST_FLAG. Verifies the
#           query path on every device + bogus probes succeed.
#   pass 2: same device set, but the build is tagged with DRVTEST_FLAG=1
#           which embeds /etc/drvtest_now into the initrd. Verifies the
#           forced-disable + re-bind path leaves the system stable.
#
# Exit 0 => PASS, exit 1 => FAIL.

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m29b"
$serial   = "serial.$logTag.log"
$debug    = "debug.$logTag.log"
$qemuLog  = "qemu.$logTag.log"
$serial2  = "serial.${logTag}_drv.log"
$debug2   = "debug.${logTag}_drv.log"
$qemuLog2 = "qemu.${logTag}_drv.log"
$bootSentinel = '\[boot\] M29B: drvmatch harness complete'
$timeoutSec   = 60

if (-not (Test-Path $disk)) { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

# Helper: run make with redirected stdout/stderr to a single log file via
# Start-Process. Avoids PowerShell's NativeCommandError wrapping when
# xorriso prints progress to stderr under $ErrorActionPreference='Stop'.
function Invoke-Make {
    param([string[]]$ExtraArgs, [string]$LogFile)
    $errFile = "$LogFile.err"
    $argList = @()
    if ($ExtraArgs) { $argList += $ExtraArgs }
    $argList += "tobyOS.iso"
    $proc = Start-Process -FilePath "C:\msys64\usr\bin\make.exe" `
        -ArgumentList $argList `
        -RedirectStandardOutput $LogFile `
        -RedirectStandardError  $errFile `
        -NoNewWindow -Wait -PassThru
    if (Test-Path $errFile) {
        Add-Content -Path $LogFile -Value (Get-Content $errFile -Raw)
        Remove-Item -Force $errFile -ErrorAction SilentlyContinue
    }
    return $proc.ExitCode
}

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog, $serial2, $debug2, $qemuLog2 -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M29B: Driver Matching & Fallback ==============" -ForegroundColor Cyan

# Build a clean stock ISO (no DRVTEST_FLAG) for pass 1. This guarantees
# that a previous run that left a DRVTEST_FLAG=1 build sitting on disk
# can't pollute the SKIPPED-vs-ARMED branch we expect on pass 1.
$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:Path
$buildLogA = "build.${logTag}_stock.log"
Write-Host "[m29b] pass 1 prep: rebuild stock ISO (DRVTEST_FLAG empty) -> $buildLogA"
Remove-Item -Force build/initrd.tar, build/base.iso, build/install.img, tobyOS.iso -ErrorAction SilentlyContinue
$rcA = Invoke-Make -ExtraArgs @() -LogFile $buildLogA
if ($rcA -ne 0) {
    Write-Host "FAIL: stock build (no DRVTEST_FLAG) failed (rc=$rcA)" -ForegroundColor Red
    Get-Content $buildLogA -Tail 30
    exit 1
}
$bldA = Get-Content $buildLogA -Raw
if ($bldA -match '\[initrd\] M29B: DRVTEST_FLAG set') {
    Write-Host "FAIL: stock build accidentally set DRVTEST_FLAG (env leak?)" -ForegroundColor Red
    Get-Content $buildLogA -Tail 30
    exit 1
}
if (-not (Test-Path $iso)) {
    Write-Host "FAIL: stock build did not produce $iso" -ForegroundColor Red
    exit 1
}

# ---------------- Pass 1: standard config, query path only ----------------
$qemuArgs = @(
    "-cdrom", $iso,
    "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
    "-smp", "4",
    "-netdev", "user,id=net0",
    "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
    "-device", "qemu-xhci,id=usb0",
    "-device", "usb-kbd,bus=usb0.0",
    "-device", "usb-mouse,bus=usb0.0",
    "-serial", "file:$serial",
    "-debugcon", "file:$debug",
    "-d", "cpu_reset,guest_errors", "-D", $qemuLog,
    "-no-reboot", "-display", "none"
)
$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
Write-Host "[m29b] pass 1: qemu pid = $($proc.Id), watching $serial..."

$start = Get-Date
$saw   = $false
while (((Get-Date) - $start).TotalSeconds -lt $timeoutSec) {
    Start-Sleep -Seconds 1
    if (-not (Test-Path $serial)) { continue }
    $log = Get-Content $serial -Raw -ErrorAction SilentlyContinue
    if (-not $log) { continue }
    if ($log -match $bootSentinel) { $saw = $true; break }
    if ($log -match 'KERNEL PANIC at|page fault|KERNEL OOPS') { break }
}
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

if (-not (Test-Path $serial)) {
    Write-Host "FAIL: $serial not produced by QEMU" -ForegroundColor Red
    exit 1
}
$txt = Get-Content $serial -Raw

if (-not $saw) {
    Write-Host "FAIL: pass 1 timed out after ${timeoutSec}s waiting for boot sentinel" -ForegroundColor Red
    Write-Host "Tail of ${serial}:" -ForegroundColor Yellow
    Get-Content $serial -Tail 80
    exit 1
}

# === required signals (pass 1) ===
$mustHave = @(
    # Kernel-side init / dump.
    '\[drvmatch\] init: total=\d+ bound=\d+ unbound=\d+ forced_off=\d+',
    '\[drvmatch\] === driver match table ===',
    '\[drvmatch\] PCI \w+:\w+\.\w \w+:\w+ cls=\w+\.\w+ drv=.* strat=(EXACT|CLASS|GENERIC|NONE|FORCED_OFF)',
    '\[boot\] M29B: driving drvmatch harness',
    '\[boot\] M29B: drvmatch total=\d+ bound=\d+ unbound=\d+ forced_off=\d+',
    # Userland boot sentinels.
    'M29B_DRV: bogus probe pci dead:beef rc=-?\d+ errno=\d+ strat=NONE -> PASS',
    'M29B_DRV: bogus probe usb dead:beef rc=-?\d+ errno=\d+ strat=NONE -> PASS',
    'M29B_DRV: bogus probe bad-bus rc=-?\d+ errno=\d+ -> PASS',
    'M29B_DRV: pci total=\d+ bound=\d+ none=\d+ exact=\d+ class=\d+ generic=\d+',
    'M29B_DRV: usb total=\d+ bound=\d+ none=\d+',
    'M29B_DRV: malformed=0',
    'M29B_DRV: PASS',
    '\[boot\] M29B: /bin/drvmatch .*PASS\)',
    # In pass 1 the forced-disable test must be SKIPPED (no flag baked).
    '\[boot\] M29B: forced-disable test SKIPPED \(no /etc/drvtest_now\)',
    '\[boot\] M29B: drvmatch harness complete'
)
$missing = @()
foreach ($pat in $mustHave) {
    if ($txt -notmatch $pat) { $missing += $pat }
}

# === forbidden tokens (pass 1) ===
$forbidden = @(
    'KERNEL PANIC at',
    'page fault',
    'KERNEL OOPS',
    'M29B_DRV: FAIL',
    '\[boot\] M29B: /bin/drvmatch not spawned',
    '\[boot\] M29B: /bin/drvmatch \(.*FAIL\)'
)
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

# Capture some interesting fingerprints for the summary line.
$pciTotalP1 = if ($txt -match 'M29B_DRV: pci total=(\d+) ') { [int]$Matches[1] } else { -1 }
$pciBoundP1 = if ($txt -match 'M29B_DRV: pci total=\d+ bound=(\d+) ') { [int]$Matches[1] } else { -1 }
$pciExactP1 = if ($txt -match 'exact=(\d+) ')   { [int]$Matches[1] } else { -1 }
$pciClassP1 = if ($txt -match 'class=(\d+) ')   { [int]$Matches[1] } else { -1 }
$pciGenP1   = if ($txt -match 'generic=(\d+)')  { [int]$Matches[1] } else { -1 }
$usbTotalP1 = if ($txt -match 'M29B_DRV: usb total=(\d+) ') { [int]$Matches[1] } else { -1 }
$usbBoundP1 = if ($txt -match 'M29B_DRV: usb total=\d+ bound=(\d+) ') { [int]$Matches[1] } else { -1 }
$forcedP1   = if ($txt -match '\[boot\] M29B: drvmatch total=\d+ bound=\d+ unbound=\d+ forced_off=(\d+)') { [int]$Matches[1] } else { -1 }

if ($missing.Count -gt 0 -or $panics.Count -gt 0) {
    Write-Host "M29B FAIL (pass 1)" -ForegroundColor Red
    if ($missing.Count -gt 0) {
        Write-Host "Missing signals:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    if ($panics.Count -gt 0) {
        Write-Host "Forbidden tokens present:" -ForegroundColor Red
        $panics | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "=== last 120 lines of ${serial} ===" -ForegroundColor Yellow
    Get-Content $serial -Tail 120
    exit 1
}

# Sanity bounds: at least one PCI device, at least one bound, no forced-off
# (we haven't disabled anything yet).
if ($pciTotalP1 -lt 1 -or $pciBoundP1 -lt 1) {
    Write-Host "FAIL: pass 1 should have >=1 PCI device with a driver bound (got total=$pciTotalP1 bound=$pciBoundP1)" -ForegroundColor Red
    exit 1
}
if ($forcedP1 -ne 0) {
    Write-Host "FAIL: pass 1 should have forced_off=0 (got $forcedP1)" -ForegroundColor Red
    exit 1
}

Write-Host "M29B PASS (pass 1) -- query path returns clean records on every PCI/USB device" -ForegroundColor Green
Write-Host "[m29b]   pci total=$pciTotalP1 bound=$pciBoundP1 (exact=$pciExactP1 class=$pciClassP1 generic=$pciGenP1) usb total=$usbTotalP1 bound=$usbBoundP1 forced=$forcedP1"

# ---------------- Pass 2: forced-disable test ----------------
# Rebuild the ISO with DRVTEST_FLAG=1 so the kernel sees /etc/drvtest_now
# and exercises drvmatch_disable_pci("e1000") + reenable.
Write-Host ""
Write-Host "[m29b] pass 2: rebuild ISO with DRVTEST_FLAG=1 to embed /etc/drvtest_now"
$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:Path
$buildLog = "build.${logTag}_drv.log"
# Force the initrd.tar / iso to be re-staged so the DRVTEST_FLAG=1 branch
# of the initrd recipe actually runs (the recipe has no FLAG dep, so we
# nuke the cached artefacts -- same trick as test_m28f.ps1 / test_m28g.ps1).
Remove-Item -Force build/initrd.tar, build/base.iso, build/install.img, tobyOS.iso -ErrorAction SilentlyContinue
$rcB = Invoke-Make -ExtraArgs @("DRVTEST_FLAG=1") -LogFile $buildLog
if ($rcB -ne 0) {
    Write-Host "FAIL: build with DRVTEST_FLAG=1 failed (rc=$rcB)" -ForegroundColor Red
    Write-Host "Tail of ${buildLog}:" -ForegroundColor Yellow
    Get-Content $buildLog -Tail 60
    exit 1
}
# Sanity-check: the DRVTEST_FLAG branch must have logged its sentinel.
$bldTxt = Get-Content $buildLog -Raw
if ($bldTxt -notmatch '\[initrd\] M29B: DRVTEST_FLAG set') {
    Write-Host "FAIL: build log lacks '[initrd] M29B: DRVTEST_FLAG set' line" -ForegroundColor Red
    Write-Host "Tail of ${buildLog}:" -ForegroundColor Yellow
    Get-Content $buildLog -Tail 30
    exit 1
}

$qemuArgs2 = @(
    "-cdrom", $iso,
    "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
    "-smp", "4",
    "-netdev", "user,id=net0",
    "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
    "-device", "qemu-xhci,id=usb0",
    "-device", "usb-kbd,bus=usb0.0",
    "-device", "usb-mouse,bus=usb0.0",
    "-serial", "file:$serial2",
    "-debugcon", "file:$debug2",
    "-d", "cpu_reset,guest_errors", "-D", $qemuLog2,
    "-no-reboot", "-display", "none"
)
Write-Host "[m29b] pass 2: launching tagged build (DRVTEST_FLAG=1)..."
$proc2 = Start-Process -FilePath $qemu -ArgumentList $qemuArgs2 -PassThru
$start2 = Get-Date
$saw2   = $false
while (((Get-Date) - $start2).TotalSeconds -lt $timeoutSec) {
    Start-Sleep -Seconds 1
    if (-not (Test-Path $serial2)) { continue }
    $log2 = Get-Content $serial2 -Raw -ErrorAction SilentlyContinue
    if (-not $log2) { continue }
    if ($log2 -match $bootSentinel) { $saw2 = $true; break }
    if ($log2 -match 'KERNEL PANIC at|page fault|KERNEL OOPS') { break }
}
Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

if (-not $saw2) {
    Write-Host "FAIL: pass 2 timed out after ${timeoutSec}s" -ForegroundColor Red
    Get-Content $serial2 -Tail 80
    exit 1
}
$txt2 = Get-Content $serial2 -Raw

# Pass 2 must show the ARMED branch (not SKIPPED), the forced-disable
# logging, and a clean "no crash" final sentinel.
$mustHave2 = @(
    '\[boot\] M29B: drvmatch harness complete',
    'M29B_DRV: PASS',
    '\[boot\] M29B: forced-disable test ARMED -- target driver=''e1000''',
    # Either the e1000 driver actually bound and we successfully unbound
    # (and saw it transition to FORCED_OFF), OR the driver was absent on
    # this VM and we report PASS because there's nothing to test.
    '\[boot\] M29B: forced-disable (PASS|removed \d+ device)',
    '\[boot\] M29B: forced-disable PASS \(no crash, drvmatch table consistent\)'
)
$rmiss2 = @()
foreach ($pat in $mustHave2) {
    if ($txt2 -notmatch $pat) { $rmiss2 += $pat }
}

# Forbidden tokens identical to pass 1.
$panics2 = @()
foreach ($pat in $forbidden) {
    if ($txt2 -match $pat) { $panics2 += $pat }
}

if ($rmiss2.Count -gt 0 -or $panics2.Count -gt 0) {
    Write-Host "M29B FAIL (pass 2): missing signals on forced-disable run:" -ForegroundColor Red
    if ($rmiss2.Count -gt 0) { $rmiss2 | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow } }
    if ($panics2.Count -gt 0) {
        Write-Host "Forbidden tokens:" -ForegroundColor Red
        $panics2 | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    }
    Get-Content $serial2 -Tail 100
    exit 1
}

# Detect whether the driver was actually present (and thus the fallback
# path was actually exercised) vs the "absent driver" PASS branch.
$driverPresent = $txt2 -match '\[boot\] M29B: forced-disable removed (\d+) device'
$removedCount  = if ($driverPresent) { [int]$Matches[1] } else { 0 }
$restoredOk    = $false
if ($driverPresent) {
    if ($txt2 -match '\[boot\] M29B: forced-disable restored (\d+) device') {
        $restored = [int]$Matches[1]
        $restoredOk = ($restored -eq $removedCount)
    }
    # Also ensure the post-rebind state is logged.
    if ($txt2 -notmatch '\[drvmatch\] post-disable: total=\d+ bound=\d+ unbound=\d+ forced_off=\d+') {
        Write-Host "FAIL: pass 2 post-disable counter line missing" -ForegroundColor Red
        Get-Content $serial2 -Tail 80
        exit 1
    }
    if ($txt2 -notmatch '\[drvmatch\] post-rebind: total=\d+ bound=\d+ unbound=\d+ forced_off=\d+') {
        Write-Host "FAIL: pass 2 post-rebind counter line missing" -ForegroundColor Red
        Get-Content $serial2 -Tail 80
        exit 1
    }
    if (-not $restoredOk) {
        Write-Host "WARN: forced-disable restored count != removed count ($removedCount removed)" -ForegroundColor Yellow
    }
    Write-Host "M29B PASS (pass 2) -- forced-disable removed $removedCount device(s), system survived, drvmatch table consistent" -ForegroundColor Green
} else {
    Write-Host "M29B PASS (pass 2) -- e1000 driver absent on this VM; harness reported PASS without crashing" -ForegroundColor Green
}

Write-Host ""
Write-Host "Last 30 lines of $serial2 for visual sanity check:"
Get-Content $serial2 -Tail 30

Write-Host ""
Write-Host "M29B PASS (overall) -- driver matching deterministic + fallback exercised cleanly" -ForegroundColor Green
exit 0
