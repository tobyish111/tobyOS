#!/usr/bin/env python3
"""Slice 39 FRONT C driver: boot the TKAPP_CHROMEWIN iso with slirp networking
+ /data disk, wait for the chromewin serial markers, and take PERIODIC QMP
screendumps so at least one catches a rendered chrome frame.

Markers on serial (all via the console->serial mirror):
  [TKAPP] chromewin ALIVE     harness spawned the host app
  [chromewin] chrome pid=...  fork+dup2+execve plumbing worked
  [chromewin] sessionId=...   DevTools bootstrap complete
  [chromewin] frame N: WxH    a screenshot was decoded and blitted
"""
import json, os, socket, subprocess, sys, time

QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
ISO = "tobyOS.iso"
DISK = "disk.img"
SERIAL = "logs/run39.log"
PORT = 4459
SHOTS = [(150, "logs/shot39_a.png"), (270, "logs/shot39_b.png")]
TOTAL = 280


def qmp(sock, obj):
    sock.sendall((json.dumps(obj) + "\r\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        ch = sock.recv(1)
        if not ch:
            break
        buf += ch
    return buf.decode(errors="replace").strip()


def main():
    os.chdir(r"C:\CustomOS\tobyOS")
    subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                   capture_output=True)
    time.sleep(1)
    for _, p in SHOTS:
        if os.path.exists(p):
            os.remove(p)
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    cmd = [QEMU, "-cdrom", ISO,
           "-drive", "file=%s,format=raw,if=ide,index=0,media=disk,cache=writethrough" % DISK,
           "-boot", "d",
           "-netdev", "user,id=net0",
           "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
           "-smp", "4", "-m", "4096", "-cpu", "qemu64,+smep,+smap",
           "-serial", "file:" + SERIAL,
           "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
           "-d", "cpu_reset", "-D", "logs/qemu39.log",
           "-no-reboot", "-display", "none"]
    print("launching qemu", flush=True)
    q = subprocess.Popen(cmd)
    sock = None
    try:
        for _ in range(60):
            try:
                sock = socket.create_connection(("127.0.0.1", PORT), timeout=2)
                break
            except OSError:
                time.sleep(0.5)
        if not sock:
            raise SystemExit("no QMP")
        # greeting
        buf = b""
        while not buf.endswith(b"\n"):
            buf += sock.recv(1)
        qmp(sock, {"execute": "qmp_capabilities"})

        start = time.time()
        shot_i = 0
        last_report = ""
        while time.time() - start < TOTAL:
            if q.poll() is not None:
                print("QEMU exited early rc=%d" % q.returncode, flush=True)
                break
            try:
                txt = open(SERIAL, errors="replace").read()
            except OSError:
                txt = ""
            for mark in ("[TKAPP] chromewin", "[chromewin] chrome pid",
                         "[devpipe]", "[chromewin] createTarget reply",
                         "[chromewin] sessionId", "[chromewin] frame"):
                if mark in txt and mark not in last_report:
                    print("%6.1fs marker: %s" % (time.time() - start, mark),
                          flush=True)
                    last_report += mark
            if shot_i < len(SHOTS) and time.time() - start >= SHOTS[shot_i][0]:
                out = SHOTS[shot_i][1].replace("\\", "/")
                r = qmp(sock, {"execute": "screendump",
                               "arguments": {"filename": out, "format": "png"}})
                print("%6.1fs screendump -> %s: %s"
                      % (time.time() - start, out, r), flush=True)
                shot_i += 1
            time.sleep(2)
        qmp(sock, {"execute": "quit"})
    finally:
        try:
            q.wait(timeout=10)
        except Exception:
            q.kill()
        subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                       capture_output=True)
    for _, p in SHOTS:
        print("shot", p, os.path.getsize(p) if os.path.exists(p) else "MISSING",
              flush=True)


if __name__ == "__main__":
    main()
