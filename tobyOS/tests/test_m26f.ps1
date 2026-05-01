# test_m26f.ps1 -- M26F HD Audio Basic Output validation.
#
# M26F replaces the M26A detect-only HDA stub with a full driver:
#
#   * src/audio_hda.c   -- CRST cycle, CORB/RIRB DMA rings, verb
#                          send/recv, codec enumeration, best-effort
#                          tone playback through output stream 0.
#   * include/tobyos/audio_hda.h
#                       -- new audio_hda_codec_at / audio_hda_tone_selftest API.
#   * src/devtest.c     -- registers the new "audio_tone" selftest.
#   * programs/audiotest/main.c
#                       -- now lists controller + per-codec records and
#                          runs both "audio" and "audio_tone" selftests.
#
# Test plan:
#
#   1. Boot QEMU with:
#        -audiodev none,id=hda
#        -device   intel-hda,id=hda0,audiodev=hda
#        -device   hda-output,bus=hda0.0,audiodev=hda
#
#      `none` is QEMU's null backend -- the HDA controller emulation
#      runs all the way through DMA + codec verbs, but no actual audio
#      ever leaves the VM. That's exactly what we want for an automated
#      check: register/verb correctness on the kernel side, no host
#      audio dependency.
#
#   2. Wait for the M26A boot sentinel (proves the harness ran).
#
#   3. Verify boot-time M26F bring-up signals:
#        - "[hda] %02x:%02x.%x ver=1.0 gcap=..." controller probe
#        - "[hda] controller reset OK"
#        - "[hda] CORB @phys ..., RIRB @phys ..."
#        - "[hda] STATESTS=0x000?" (one or more codec bits set)
#        - "[hda] codec 0: vendor=..."
#        - "[hda]   AFG @NID ..."
#        - "[hda]   widgets: ?? DAC, ?? ADC, ?? PIN, ..."
#        - "[hda] enumeration complete: \d+ codec(s)"
#
#   4. Verify userland audiotest signals:
#        - "INFO: audiotest: \d+ AUDIO record(s) -- 1 controller + \d+ codec(s)"
#        - "PASS: audiotest controller: HDA ver=1.0 ..."
#        - "PASS: audiotest tone: tone @880Hz, SD\d+ stream1 fmt=0x0011 ..."
#
#   5. Confirm no kernel panic / page fault / unbound MSI vector.
#
# Exit 0 => PASS, exit 1 => FAIL with a list of missing patterns.


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$logTag   = "m26f"
$serial   = Join-Path $LogDir "serial.$logTag.log"
$debug    = Join-Path $LogDir "debug.$logTag.log"
$qemuLog  = Join-Path $LogDir "qemu.$logTag.log"
$bootSentinel  = '\[boot\] M26A: userland .* PASS .* of \d+'
$bootTimeoutSec = 90

if (-not (Test-Path $iso))  { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============== M26F: HD Audio Basic Output ==============" -ForegroundColor Cyan

$qemuArgs = @(
    "-cdrom", $iso,
    "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
    "-smp", "4",
    "-netdev", "user,id=net0",
    "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
    # M26B carry-over: keep xHCI + hub + a couple of HID devs in the
    # config so a regression in HDA can't be confused with regression
    # in unrelated USB-class drivers.
    "-device", "qemu-xhci,id=usb0",
    "-device", "usb-kbd,bus=usb0.0",
    "-device", "usb-mouse,bus=usb0.0",
    # M26F: HD Audio. `none` backend = no host audio, no host driver
    # required, full codec emulation still runs inside QEMU. We use
    # hda-duplex (bidir codec) rather than hda-output because some
    # QEMU builds don't expose hda-output as a "discoverable" codec
    # on STATESTS unless an explicit bus= is given, while hda-duplex
    # auto-attaches to the only intel-hda bus and reliably asserts
    # its SDI presence bit.
    "-audiodev", "none,id=hda",
    "-device",   "intel-hda,id=hda0",
    "-device",   "hda-duplex,audiodev=hda",
    "-serial", "file:$serial",
    "-debugcon", "file:$debug",
    "-d", "cpu_reset,guest_errors", "-D", $qemuLog,
    "-no-reboot", "-display", "none"
)
$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
Write-Host "[m26f] qemu pid = $($proc.Id), watching $serial for boot sentinel..."

# ---- Wait for boot sentinel ----
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

# Give the box a moment to flush any tail output before we kill it.
Start-Sleep -Milliseconds 1000
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

if (-not (Test-Path $serial)) {
    Write-Host "FAIL: $serial not produced by QEMU" -ForegroundColor Red
    exit 1
}
$txt = Get-Content $serial -Raw

if (-not $saw) {
    Write-Host "FAIL: timed out after ${bootTimeoutSec}s waiting for boot sentinel" -ForegroundColor Red
    Write-Host "Tail of ${serial}:" -ForegroundColor Yellow
    Get-Content $serial -Tail 80
    exit 1
}

# === required signals ===
$mustHave = @(
    # --- controller probe + bring-up ---
    "\[hda\] [0-9a-f]{2}:[0-9a-f]{2}\.\d ver=\d+\.\d+ gcap=0x[0-9a-f]+ ISS=\d+ OSS=\d+",
    "\[hda\] controller reset OK",
    "\[hda\] CORB @phys 0x[0-9a-f]+, RIRB @phys 0x[0-9a-f]+ \(rings live\)",

    # --- codec scan + walk ---
    "\[hda\] STATESTS=0x[0-9a-f]+ \(latched post-CRST presence bitmap, live=0x[0-9a-f]+\)",
    "\[hda\] codec 0: vendor=0x[0-9a-f]+ device=0x[0-9a-f]+",
    "\[hda\]   AFG @NID \d+: widgets \d+\.\.\d+ \(\d+ total\)",
    "\[hda\]   widgets: \d+ DAC, \d+ ADC, \d+ PIN,",
    "\[hda\] enumeration complete: \d+ codec\(s\)",

    # --- userland audiotest ---
    'INFO: audiotest: \d+ AUDIO record\(s\) -- 1 controller \+ \d+ codec\(s\)',
    "PASS: audiotest controller: HDA ver=\d+\.\d+ gcap=0x[0-9a-f]+ ISS=\d+ OSS=\d+ \d+ codec\(s\)",
    "PASS: audiotest tone: tone @880Hz, SD\d+ stream1 fmt=0x0011 cbl=\d+",

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
               '\[FAIL\] audio:', '\[FAIL\] audio_tone:',
               '\[hda\] verb timeout',
               '\[hda\] CRST never',
               '\[hda\] CORB/RIRB DMA alloc failed')
$panics = @()
foreach ($pat in $forbidden) {
    if ($txt -match $pat) { $panics += $pat }
}

Write-Host ""
if ($missing.Count -eq 0 -and $panics.Count -eq 0) {
    Write-Host "M26F PASS -- HDA controller initialised, codecs enumerated, tone path exercised." -ForegroundColor Green
    Write-Host ""
    Write-Host "Controller probe + reset:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '\[hda\] (?:[0-9a-f]+:|controller|CORB|STATESTS|enumeration)' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "Codec topology:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '\[hda\] (?:codec |  AFG|  widgets:)' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ""
    Write-Host "Userland audiotest result:" -ForegroundColor Cyan
    Select-String -Path $serial -Pattern '(?:INFO|PASS|SKIP|FAIL): audiotest' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    exit 0
} else {
    Write-Host "M26F FAIL" -ForegroundColor Red
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
