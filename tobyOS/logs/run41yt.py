#!/usr/bin/env python3
"""Phase 2 WHPX probe: chromewin pointed at youtube.com. More RAM (YouTube is
heavy: the TCG run hit ~3.9GB), longer run, own output paths. Derived from
run39whpx.py."""
import json, os, socket, subprocess, sys, time

QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
ISO = "tobyOS.iso"
DISK = "disk.img"
SERIAL = "logs/run41yt.log"
PORT = 4461
SHOTS = [(90, "logs/ytwx_a.png"), (200, "logs/ytwx_b.png"),
         (350, "logs/ytwx_c.png")]
TOTAL = 360


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
           "-accel", "whpx,kernel-irqchip=off",
           "-smp", "4", "-m", "6144", "-cpu", "qemu64,+smep,+smap",
           "-serial", "file:" + SERIAL,
           "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
           "-no-reboot", "-display", "none"]
    print("launching qemu (WHPX)", flush=True)
    q = subprocess.Popen(cmd, stderr=subprocess.PIPE)
    sock = None
    try:
        for _ in range(60):
            if q.poll() is not None:
                err = q.stderr.read().decode(errors="replace")
                raise SystemExit("QEMU exited rc=%d: %s" % (q.returncode, err[:500]))
            try:
                sock = socket.create_connection(("127.0.0.1", PORT), timeout=2)
                break
            except OSError:
                time.sleep(0.5)
        if not sock:
            raise SystemExit("no QMP")
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
