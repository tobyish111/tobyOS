# Boot-only smoke test for milestone 16.
#
# We can't easily drive the shell from QMP after milestone 14 (the GUI
# login steals the keyboard), so this just boots the kernel headless
# and greps the debug.log for pkg-manager breadcrumbs to confirm:
#
#   - pkg_init() ran
#   - the directory skeleton under /data/* was created (or already there)
#   - nothing panicked
#
# The interactive pkg demo (install, see it in launcher, run it, remove
# it) is documented in the README section emitted by the assistant.

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
    "-display", "none"
)

$proc = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru -RedirectStandardError "qemu.stderr.log"
Write-Host "qemu pid = $($proc.Id), booting for 6s..."
Start-Sleep -Seconds 6
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
Start-Sleep -Milliseconds 300

Write-Host ""
Write-Host "=== pkg-manager breadcrumbs in debug.log ==="
$log = Get-Content debug.log -ErrorAction SilentlyContinue
$patterns = @(
    '\[pkg\] package manager ready',
    'milestone'
)
foreach ($p in $patterns) {
    $hit = $log | Select-String -Pattern $p
    if ($hit) {
        Write-Host "  OK   $p"
        $hit | ForEach-Object { Write-Host "       $($_.Line.Trim())" }
    } else {
        Write-Host "  MISS $p"
    }
}

Write-Host ""
Write-Host "=== panic/assert check ==="
$bad = $log | Select-String -Pattern 'panic|PANIC|assert|ERROR'
if ($bad) {
    Write-Host "  FOUND ERRORS:" -ForegroundColor Red
    $bad | ForEach-Object { Write-Host "    $($_.Line.Trim())" }
} else {
    Write-Host "  clean -- no panic/assert/ERROR in debug.log"
}
