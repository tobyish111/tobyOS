#!/usr/bin/env python3
"""Headless QEMU + QMP screendump driver for tobyOS GUI verification.

Reusable across the unify-on-TobyTK app migrations (and any GUI work): it boots
the ISO with -display none, waits for a serial marker, takes a PNG screendump
via QMP, then quits QEMU. Pairs with the `-DTKAPP_BOOT` boot harness in
src/kernel.c, which auto-logs-in, launches one TobyTK app (TKAPP_PATH/NAME, with
optional typed TKAPP_KEYS), and prints "[TKAPP] <name> ... holding ~8s for
screenshot" once it's up -- that is the default --marker this script waits for.

Typical workflow:
  1. Point the harness at the app: edit the TKAPP_PATH/TKAPP_NAME (and TKAPP_KEYS)
     #defines in the TKAPP_BOOT block of src/kernel.c.
  2. Build (MSYS2 UCRT64), e.g.:
       export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"; mkdir -p /c/t
       make "CC=TMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' gcc" \
            EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT" iso
  3. Capture:
       python tools/gui_shot.py --iso tobyOS.iso --disk disk.img \
              --out /c/t/shot.png --serial /c/t/serial.log
  4. Open the PNG (the agent can Read it) to confirm the app renders.

Notes:
  - QEMU 10.x supports `screendump ... format=png` directly (no PPM conversion).
  - TCG boot is slow; default --timeout is generous. Quiesce the host (don't run
    a build concurrently) or capture can take minutes.
  - --keys-marker can wait on a different serial string if you use a custom harness.
"""
import argparse, socket, subprocess, sys, time, os, json


def qmp_connect(host, port, tries=120):
    for _ in range(tries):
        try:
            return socket.create_connection((host, port), timeout=2)
        except OSError:
            time.sleep(0.5)
    raise SystemExit("QMP: could not connect to %s:%d" % (host, port))


def qmp_readline(sock):
    line = b""
    while not line.endswith(b"\n"):
        ch = sock.recv(1)
        if not ch:
            break
        line += ch
    return line.decode(errors="replace").strip()


def qmp_send(sock, obj):
    sock.sendall((json.dumps(obj) + "\r\n").encode())
    return qmp_readline(sock)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True, help="path to tobyOS.iso")
    ap.add_argument("--out", required=True, help="output PNG path")
    ap.add_argument("--serial", required=True, help="serial log path (also the marker source)")
    ap.add_argument("--disk", default="", help="optional disk.img (mounted /data)")
    ap.add_argument("--marker", default="holding",
                    help="serial substring to wait for before capturing")
    ap.add_argument("--timeout", type=int, default=220, help="seconds to wait for the marker")
    ap.add_argument("--settle", type=float, default=2.0, help="seconds to wait after the marker")
    ap.add_argument("--qemu", default=r"C:\Program Files\qemu\qemu-system-x86_64.exe")
    ap.add_argument("--port", type=int, default=4455, help="QMP TCP port")
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", default="512")
    args = ap.parse_args()

    out = args.out.replace("\\", "/")
    for p in (args.out, args.serial):
        if os.path.exists(p):
            try:
                os.remove(p)
            except OSError:
                pass

    cmd = [args.qemu, "-cdrom", args.iso, "-boot", "d",
           "-smp", str(args.smp), "-m", args.mem, "-display", "none",
           "-serial", "file:" + args.serial,
           "-qmp", "tcp:127.0.0.1:%d,server,nowait" % args.port,
           "-no-reboot"]
    if args.disk and os.path.exists(args.disk):
        cmd[3:3] = ["-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % args.disk]

    print("launching:", " ".join(cmd), flush=True)
    qemu = subprocess.Popen(cmd)
    try:
        sock = qmp_connect("127.0.0.1", args.port)
        qmp_readline(sock)                                  # greeting
        print("qmp caps:", qmp_send(sock, {"execute": "qmp_capabilities"}), flush=True)

        deadline = time.time() + args.timeout
        seen = False
        while time.time() < deadline:
            if os.path.exists(args.serial):
                try:
                    if args.marker in open(args.serial, "r", errors="replace").read():
                        seen = True
                        break
                except OSError:
                    pass
            if qemu.poll() is not None:
                print("QEMU exited early, code", qemu.returncode, flush=True)
                break
            time.sleep(1)
        print("marker '%s' seen:" % args.marker, seen, flush=True)

        time.sleep(args.settle)
        print("screendump:", qmp_send(sock, {"execute": "screendump",
              "arguments": {"filename": out, "format": "png"}}), flush=True)
        time.sleep(1)
        qmp_send(sock, {"execute": "quit"})
    finally:
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()

    ok = os.path.exists(args.out) and os.path.getsize(args.out) > 0
    print("RESULT:", "OK" if ok else "NO-IMAGE", args.out, flush=True)
    sys.exit(0 if ok else 2)


if __name__ == "__main__":
    main()
