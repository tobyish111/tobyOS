# test_m26g.ps1 -- M26G ACPI Battery Support validation.
#
# M26G replaces the M26A "always-absent stub" battery driver with two
# cooperating discovery paths:
#
#   * Heuristic AML byte scan -- searches DSDT + every SSDT for the
#     literal "PNP0C0A" _HID. A hit means the firmware DECLARED a
#     battery; reading the actual charge needs a real AML interpreter
#     (out of scope for M26G). On QEMU x86 (no native ACPI battery
#     device) this finds nothing -- which is the correct, graceful
#     "no battery" outcome.
#
#   * fw_cfg mock injection -- the kernel reads
#     "opt/tobyos/battery_mock" from QEMU's fw_cfg interface and, if
#     present, treats it as the live battery state. This is how this
#     test script forces a "battery present, charging, 75%" path on
#     a virtualised desktop.
#
# Test plan:
#
#   Run A: no fw_cfg mock provided. Expect:
#     - kernel logs "[fw_cfg] QEMU fw_cfg interface present"
#     - kernel logs "[battery] no ACPI battery detected"
#     - userland: "INFO: batterytest: no battery devices reported by kernel"
#     - userland: "SKIP: batterytest: no ACPI battery detected ..."
#     - boot SHOULD NOT panic
#
#   Run B: -fw_cfg name=opt/tobyos/battery_mock,string=state=charging,percent=75,...
#     Expect:
#     - kernel logs "[battery] fw_cfg mock injected: state=2 percent=75 ..."
#     - userland: one battery row with "75% charging" in the extra column
#     - userland: "PASS: batterytest: battery0: 75% charging"
#     - boot SHOULD NOT panic
#
# Exit 0 => both runs PASS, exit 1 => any failure.


$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot
$LogDir = Join-Path $RepoRoot "logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

$ErrorActionPreference = 'Stop'

$qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$iso      = "tobyOS.iso"
$disk     = "disk.img"
$bootSentinel  = '\[boot\] M26A: userland .* PASS .* of \d+'
$bootTimeoutSec = 90

if (-not (Test-Path $iso))  { Write-Host "ISO not built -- run 'make' first" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $disk)) { Write-Host "$disk missing -- run 'make disk.img' first" -ForegroundColor Red; exit 1 }

function Run-Scenario {
    param(
        [string]   $Name,
        [string]   $Tag,
        [string[]] $ExtraQemuArgs,
        [string[]] $MustHave,
        [string[]] $MustNotHave
    )

    $serial   = Join-Path $LogDir "serial.$Tag.log"
    $debug    = Join-Path $LogDir "debug.$Tag.log"
    $qemuLog  = Join-Path $LogDir "qemu.$Tag.log"

    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    Remove-Item -Force $serial, $debug, $qemuLog -ErrorAction SilentlyContinue

    Write-Host ""
    Write-Host "============== $Name ==============" -ForegroundColor Cyan

    $qemuArgs = @(
        "-cdrom", $iso,
        "-drive", "file=$disk,format=raw,if=ide,index=0,media=disk",
        "-smp", "4",
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
        # Keep the M26B+ USB stack alive so a regression in fw_cfg /
        # battery can't be confused with a USB regression.
        "-device", "qemu-xhci,id=usb0",
        "-device", "usb-kbd,bus=usb0.0",
        "-device", "usb-mouse,bus=usb0.0",
        "-serial", "file:$serial",
        "-debugcon", "file:$debug",
        "-d", "cpu_reset,guest_errors", "-D", $qemuLog,
        "-no-reboot", "-display", "none"
    ) + $ExtraQemuArgs

    $proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru
    Write-Host "[$Tag] qemu pid = $($proc.Id), watching $serial for boot sentinel..."

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

    Start-Sleep -Milliseconds 1000
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500

    if (-not (Test-Path $serial)) {
        Write-Host "FAIL: $serial not produced by QEMU" -ForegroundColor Red
        return $false
    }
    $txt = Get-Content $serial -Raw

    if (-not $saw) {
        Write-Host "FAIL: timed out after ${bootTimeoutSec}s waiting for boot sentinel" -ForegroundColor Red
        Write-Host "Tail of ${serial}:" -ForegroundColor Yellow
        Get-Content $serial -Tail 80
        return $false
    }

    $missing = @()
    foreach ($pat in $MustHave) {
        if ($txt -notmatch $pat) { $missing += $pat }
    }
    $forbidden = @('panic', 'page fault', 'KERNEL OOPS') + $MustNotHave
    $found = @()
    foreach ($pat in $forbidden) {
        if ($txt -match $pat) { $found += $pat }
    }

    if ($missing.Count -eq 0 -and $found.Count -eq 0) {
        Write-Host "$Name PASS" -ForegroundColor Green
        Write-Host "Battery + fw_cfg signals:" -ForegroundColor Cyan
        Select-String -Path $serial -Pattern '\[(?:fw_cfg|battery)\]' |
            ForEach-Object { Write-Host "  $($_.Line)" }
        Write-Host "Userland batterytest:" -ForegroundColor Cyan
        Select-String -Path $serial -Pattern '(?:INFO|PASS|SKIP|FAIL): batterytest' |
            ForEach-Object { Write-Host "  $($_.Line)" }
        return $true
    } else {
        Write-Host "$Name FAIL" -ForegroundColor Red
        if ($missing.Count -gt 0) {
            Write-Host "Missing signals:" -ForegroundColor Red
            $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
        }
        if ($found.Count -gt 0) {
            Write-Host "Forbidden tokens present:" -ForegroundColor Red
            $found | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
        }
        Write-Host ""
        Write-Host "=== last 200 lines of ${serial} ===" -ForegroundColor Yellow
        Get-Content $serial -Tail 200
        return $false
    }
}

# ---- Run A: absent path ----
$absentPass = Run-Scenario `
    -Name "M26G(A): no battery (no fw_cfg mock)" `
    -Tag  "m26g_a" `
    -ExtraQemuArgs @() `
    -MustHave @(
        '\[fw_cfg\] QEMU fw_cfg interface present at 0x510/0x511',
        '\[battery\] no ACPI battery detected \(no fw_cfg mock, no PNP0C0A in AML\)',
        'INFO: batterytest: no battery devices reported by kernel',
        'SKIP: batterytest: no ACPI battery detected',
        # Regression guards: prior M26 phases must still pass.
        '\[boot\] M26A: peripheral inventory \+ self-tests',
        '\[boot\] M26A: userland (\d+) PASS / 0 FAIL / 0 missing of \1'
    ) `
    -MustNotHave @('FAIL: batterytest:')

# ---- Run B: mock battery ----
# fw_cfg string must not contain commas because the QEMU -fw_cfg
# parser uses comma to separate sub-options. We use semicolons.
$mockString = "state=charging;percent=75;design=50000;remaining=37500;rate=1500"
$mockPass = Run-Scenario `
    -Name "M26G(B): fw_cfg mock battery (charging 75%)" `
    -Tag  "m26g_b" `
    -ExtraQemuArgs @(
        "-fw_cfg", "name=opt/tobyos/battery_mock,string=$mockString"
    ) `
    -MustHave @(
        '\[fw_cfg\] QEMU fw_cfg interface present at 0x510/0x511',
        '\[battery\] fw_cfg mock injected: state=2 percent=75 design=50000 remaining=37500 rate=1500',
        'INFO: batterytest: 1 battery device\(s\) reported',
        'battery\s+battery0\s+acpi_bat\(mock\).*75% charging \(37500/50000 mWh, 1500 mW\)',
        'PASS: batterytest: battery0: 75% charging',
        '\[boot\] M26A: peripheral inventory \+ self-tests',
        '\[boot\] M26A: userland (\d+) PASS / 0 FAIL / 0 missing of \1'
    ) `
    -MustNotHave @('SKIP: batterytest:', 'FAIL: batterytest:')

Write-Host ""
Write-Host "================== M26G summary ==================" -ForegroundColor Cyan
$summary = @(
    [PSCustomObject]@{ Scenario='A: absent (no mock)';        Verdict = if ($absentPass) {'PASS'} else {'FAIL'} },
    [PSCustomObject]@{ Scenario='B: fw_cfg mock 75% charging'; Verdict = if ($mockPass)   {'PASS'} else {'FAIL'} }
)
$summary | Format-Table -AutoSize | Out-String | Write-Host

if ($absentPass -and $mockPass) {
    Write-Host "M26G PASS -- battery driver detects absence cleanly AND honours fw_cfg mock." -ForegroundColor Green
    exit 0
} else {
    Write-Host "M26G FAIL -- one or more scenarios did not pass (see logs above)." -ForegroundColor Red
    exit 1
}
