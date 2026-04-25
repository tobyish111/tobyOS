# test_m24d.ps1 -- end-to-end driver for the milestone-24D HTTP +
# package-install self-test.
#
# What it does, in order:
#   1. Stage tobyOS/build/m24d/ with /m24d_smoke.txt and helloapp.tpkg
#      (copied from $(HELLOAPP_TPKG)).
#   2. Start `py -m http.server 8000` in the background, serving from
#      that directory.
#   3. Build the kernel with -DHTTP_M24D_SELFTEST (`make m24dtest`).
#   4. Run the kernel (`make run`), wait for "[m24d-selftest] SUCCESS"
#      or "[m24d-selftest] FAIL" in serial.log.
#   5. Stop QEMU + the http server, print the verdict, exit non-zero
#      on failure.
#
# Run from the tobyOS directory:
#   powershell -ExecutionPolicy Bypass -File tools\test_m24d.ps1

# NOTE: we intentionally do NOT set $ErrorActionPreference = "Stop"
# because `make` runs sub-tools (xorriso, limine bios-install) that
# write informational text to stderr, which PowerShell would otherwise
# turn into terminating exceptions. We check $LASTEXITCODE instead.

# Helper: run `make` with the given target list, swallow stderr-as-error
# noise (xorriso/limine emit info on stderr), fail only on non-zero exit.
# Throws on failure; otherwise returns nothing.
function Invoke-Make {
    param([string[]]$Targets, [string]$Cwd)
    Push-Location $Cwd
    try {
        # *>&1 merges every PowerShell stream (success/error/warning/...)
        # into the success stream so the assignment below captures the
        # full output without PS treating stderr as a fatal exception.
        $output = & make $Targets *>&1
        $rc     = $LASTEXITCODE
        if ($rc -ne 0) {
            $output | ForEach-Object { Write-Host $_ }
            throw "make $($Targets -join ' ') failed (exit $rc)"
        }
    } finally { Pop-Location }
}

$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$stage = Join-Path $build "m24d"

Write-Host "[m24d-driver] staging $stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Smoke text file: exactly 15 ASCII bytes including LF, matched
# byte-for-byte by http_m24d_selftest().
[System.IO.File]::WriteAllBytes(
    (Join-Path $stage "m24d_smoke.txt"),
    [System.Text.Encoding]::ASCII.GetBytes("tobyOS-m24d-ok`n"))

# helloapp.tpkg comes from the standard build. Build it first so the
# test driver doesn't fail with "file not found" the first time round.
Write-Host "[m24d-driver] make helloapp-tpkg"
Invoke-Make -Targets @("helloapp-tpkg") -Cwd $root

Copy-Item (Join-Path $build "helloapp.tpkg") $stage -Force
Write-Host "[m24d-driver] staged:"
Get-ChildItem $stage | ForEach-Object {
    Write-Host ("    {0,-22} {1,7} bytes" -f $_.Name, $_.Length)
}

# 2. Start the host HTTP server. Listen only on 127.0.0.1 so we don't
#    expose the test files to the LAN; SLIRP user-net translates 10.0.2.2
#    inside the guest to 127.0.0.1 on the host.
Write-Host "[m24d-driver] starting py -m http.server 8000 (cwd=$stage)"
$serverProc = Start-Process -FilePath "py" `
    -ArgumentList @("-m", "http.server", "8000", "--bind", "127.0.0.1") `
    -WorkingDirectory $stage `
    -PassThru `
    -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $stage "server.out.log") `
    -RedirectStandardError  (Join-Path $stage "server.err.log")
Start-Sleep -Seconds 1
Write-Host "[m24d-driver] server pid=$($serverProc.Id)"

$success = $false
try {
    # 3. Build the self-test kernel.
    Write-Host "[m24d-driver] make m24dtest"
    Invoke-Make -Targets @("m24dtest") -Cwd $root

    # 4. Run + watch serial.log.
    $serialPath = Join-Path $root "serial.log"
    if (Test-Path $serialPath) { Remove-Item -Force $serialPath }

    Write-Host "[m24d-driver] starting QEMU (make run, backgrounded)"
    $qemu = Start-Process -FilePath "make" `
        -ArgumentList @("run") `
        -WorkingDirectory $root `
        -PassThru `
        -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $stage "make_run.out.log") `
        -RedirectStandardError  (Join-Path $stage "make_run.err.log")
    Write-Host "[m24d-driver] QEMU pid=$($qemu.Id) -- waiting for selftest result..."

    $deadline = (Get-Date).AddSeconds(45)
    $verdict  = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if (Test-Path $serialPath) {
            $matches = Select-String -Path $serialPath `
                -Pattern '\[m24d-selftest\] (SUCCESS|FAIL)' `
                -ErrorAction SilentlyContinue
            if ($matches) {
                $verdict = $matches[0].Line
                break
            }
        }
    }

    if ($verdict) {
        Write-Host ""
        Write-Host "[m24d-driver] verdict line: $verdict"
        $success = $verdict -match "SUCCESS"
    } else {
        Write-Host "[m24d-driver] TIMEOUT: no selftest verdict after 45s"
    }

    Write-Host ""
    Write-Host "==== relevant serial.log lines ===="
    if (Test-Path $serialPath) {
        Select-String -Path $serialPath `
            -Pattern '\[m24d-selftest\]|\[http\]|\[pkg\]|\[dns\]\s+(query|reply)' |
            ForEach-Object { Write-Host ("   " + $_.Line) }
    }
}
finally {
    Write-Host ""
    Write-Host "[m24d-driver] cleanup: stopping QEMU + HTTP server"
    Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
    if ($serverProc -and -not $serverProc.HasExited) {
        Stop-Process -Id $serverProc.Id -Force
    }
    Get-Process py -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -eq $serverProc.Path
    } | Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($success) {
    Write-Host ""
    Write-Host "[m24d-driver] PASS"
    exit 0
} else {
    Write-Host ""
    Write-Host "[m24d-driver] FAIL"
    exit 1
}
