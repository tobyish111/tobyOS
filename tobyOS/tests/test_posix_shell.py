#!/usr/bin/env python3
"""POSIX shell conformance smoke -- Linux/QEMU driver.

Same test as tests/test_posix_shell.ps1, runnable where PowerShell is not.
The list of required sentinels is READ OUT OF THE .ps1 so the two drivers
cannot drift: add a case in one place and both check it.

    make posixshtest && python3 tests/test_posix_shell.py

Options:
    --rebuild   run `make posixshtest` first
    --timeout N seconds to wait for the boot to reach the PASS marker
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERIAL = ROOT / "logs" / "serial.log"
PASS_MARKER = "POSIXSH: PASS"

# Sentinels that must NOT appear at the start of a line: each is a branch the
# shell should not have taken.
FORBIDDEN = [
    "if-bad", "elif-bad", "case-bad", "exit-bad", "trap-reset-bad",
    "case-wild", "loop-later", "return-bad", "subshell-alias-bad",
    "subshell-function-bad", "subshell-exit-bad", "errexit-bad",
    "empty-at-bad", "case-q-bad", "case-cls-bad", "case-neg-bad",
    "plain-bad", "clobber-bad", "ml-if-bad", "ml-case-bad",
    "case-quoted-bad", "errexit-fn-bad", "never",
]


def required_patterns():
    """Pull the $required list out of the PowerShell driver."""
    ps1 = (ROOT / "tests" / "test_posix_shell.ps1").read_text()
    block = ps1.split("$required = @(", 1)[1].split("\n)", 1)[0]
    out = []
    for line in block.splitlines():
        line = line.strip().rstrip(",")
        if not line or line.startswith("#"):
            continue
        if line.startswith("'") and line.endswith("'"):
            out.append(line[1:-1].replace("''", "'"))
        elif line.startswith('"') and line.endswith('"'):
            out.append(line[1:-1])
    return out


def boot(timeout):
    SERIAL.parent.mkdir(parents=True, exist_ok=True)
    for name in ("serial.log", "debug.log", "qemu.log"):
        (SERIAL.parent / name).unlink(missing_ok=True)

    argv = [
        "qemu-system-x86_64",
        "-cdrom", str(ROOT / "tobyOS.iso"),
        "-drive", f"file={ROOT / 'disk.img'},format=raw,if=ide,index=0,media=disk",
        "-smp", "4", "-m", "1024",
        "-serial", f"file:{SERIAL}",
        "-debugcon", f"file:{SERIAL.parent / 'debug.log'}",
        "-d", "cpu_reset,guest_errors", "-D", str(SERIAL.parent / "qemu.log"),
        "-no-reboot", "-no-shutdown", "-display", "none",
    ]
    proc = subprocess.Popen(argv, cwd=ROOT, start_new_session=True)
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            time.sleep(1)
            if SERIAL.exists() and PASS_MARKER in SERIAL.read_text(errors="replace"):
                break
            if proc.poll() is not None:
                break
    finally:
        if proc.poll() is None:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        proc.wait()
    time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument("--timeout", type=int, default=200)
    args = ap.parse_args()

    if args.rebuild:
        rc = subprocess.call(["make", "-j4", "posixshtest"], cwd=ROOT)
        if rc != 0:
            return rc

    boot(args.timeout)
    if not SERIAL.exists():
        print("=== POSIX shell smoke: FAIL === (no serial output)")
        return 1

    raw = SERIAL.read_text(errors="replace")
    # The harness echoes each command it drives, and those echoes contain the
    # sentinels the commands are meant to PRINT. Assert on real output only.
    out = "\n".join(l for l in raw.splitlines() if "[shell-test] $ " not in l)

    missing = [p for p in required_patterns() if p not in out]
    for name in FORBIDDEN:
        if re.search(r"(?m)^POSIXSH: " + re.escape(name), out):
            missing.append(f"unexpected POSIXSH: {name}")
    if "posixsh_a.txt" not in out or "posixsh_b.txt" not in out:
        missing.append("glob expansion did not show both posixsh files")

    if missing:
        print("=== POSIX shell smoke: FAIL ===")
        for m in missing:
            print(f"missing/bad: {m}")
        return 1

    print("[posixsh] OVERALL: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
