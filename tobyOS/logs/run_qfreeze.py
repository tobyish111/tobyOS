#!/usr/bin/env python3
"""Slice 92: A/B runner for the CoW-fork STW deadlock guard (linux-qfreeze).

Boots the CONSOLE flavour (-DCHROMIUM_BOOT, no TKAPP) under WHPX -smp 4 and
watches serial for the [QFREEZETEST] VERDICT line. Outcomes:

  PASS      -- fork+syscall-hammer survived; the park/BKL cycle is closed.
  FAIL/ERR  -- test-level failure (lost write / setup error): investigate.
  WEDGE     -- no verdict: either total serial silence or heartbeats with
               [bkl] acq=0. On the pre-slice-92 kernel this is the expected
               outcome (the deadlock). Registers are captured via QMP for
               both shapes (logs/qfreeze_regs.txt).

Usage: SMP=4 RUNS=3 python logs/run_qfreeze.py
"""
import json, os, re, socket, subprocess, time

QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
ISO = "tobyOS.iso"
DISK = "disk.img"
SERIAL = "logs/run_qfreeze.log"
PORT = 4467
RUN_SECS = 600          # hard cap per run
VERDICT_WAIT = 300      # after QFREEZETEST spawn line, verdict must appear
STALL_SECS = 25


class Qmp:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.f = self.sock.makefile("rwb")
        json.loads(self.f.readline())
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        req = {"execute": name}
        if args:
            req["arguments"] = args
        self.f.write((json.dumps(req) + "\r\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return None
            msg = json.loads(line)
            if "return" in msg or "error" in msg:
                return msg

    def hmp(self, cl):
        r = self.cmd("human-monitor-command", **{"command-line": cl})
        return r.get("return", "") if r else ""


def capture(qmp, tag):
    with open("logs/qfreeze_regs.txt", "a") as out:
        out.write("\n=== %s ===\n=== capture 1 ===\n" % tag)
        out.write(qmp.hmp("info registers -a"))
        time.sleep(3)
        out.write("\n=== capture 2 (3s later; same RIPs = deadlock) ===\n")
        out.write(qmp.hmp("info registers -a"))


def one_run(idx):
    subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                   capture_output=True)
    time.sleep(1)
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    smp = os.environ.get("SMP", "4")
    cmd = [QEMU, "-cdrom", ISO,
           "-drive", "file=%s,format=raw,if=none,id=hd0,snapshot=on" % DISK,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-netdev", "user,id=net0",
           "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
           "-accel", "whpx,kernel-irqchip=off",
           "-smp", smp, "-m", "8192", "-cpu", "qemu64,+smep,+smap",
           "-serial", "file:" + SERIAL,
           "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
           "-no-reboot", "-display", "none"]
    print("run %d: launching (smp=%s)" % (idx, smp), flush=True)
    q = subprocess.Popen(cmd, stderr=subprocess.PIPE)
    qmp = None
    verdict = "NONE"
    try:
        for _ in range(60):
            if q.poll() is not None:
                raise SystemExit("QEMU exited rc=%d" % q.returncode)
            try:
                qmp = Qmp(PORT)
                break
            except OSError:
                time.sleep(0.5)
        if not qmp:
            raise SystemExit("no QMP")

        start = time.time()
        spawn_seen = None
        armed = False           # stall detection only after the kernel talks
        last_size = -1
        last_growth = time.time()
        while time.time() - start < RUN_SECS:
            time.sleep(2)
            if q.poll() is not None:
                print("run %d: QEMU exited early" % idx, flush=True)
                break
            try:
                blob = open(SERIAL, "rb").read()
                size = len(blob)
            except OSError:
                blob, size = b"", -1
            if not armed and b"[hb " in blob:
                armed = True
                print("run %d: armed (kernel heartbeat seen)" % idx,
                      flush=True)

            m = re.search(rb"\[QFREEZETEST\] VERDICT: (\S+) exit=(-?\d+)", blob)
            if m:
                verdict = "%s exit=%s" % (m.group(1).decode(),
                                          m.group(2).decode())
                print("run %d: VERDICT %s" % (idx, verdict), flush=True)
                break
            if spawn_seen is None and b"QFREEZETEST: spawning" in blob:
                spawn_seen = time.time()
                print("run %d: qfreeze spawned" % idx, flush=True)

            if size != last_size:
                last_size = size
                last_growth = time.time()
            elif armed and time.time() - last_growth > STALL_SECS:
                print("run %d: SERIAL SILENT %ds -- total wedge, capturing"
                      % (idx, STALL_SECS), flush=True)
                capture(qmp, "run %d total-silence wedge" % idx)
                verdict = "WEDGE(silent)"
                break

            if spawn_seen and time.time() - spawn_seen > VERDICT_WAIT:
                print("run %d: NO VERDICT %ds after spawn (serial %s) -- "
                      "capturing" % (idx, VERDICT_WAIT,
                                     "alive" if size != last_size else "quiet"),
                      flush=True)
                capture(qmp, "run %d no-verdict wedge" % idx)
                verdict = "WEDGE(no-verdict)"
                break
        return verdict
    finally:
        try:
            qmp and qmp.cmd("quit")
        except Exception:
            pass
        try:
            q.wait(timeout=10)
        except Exception:
            q.kill()
        subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                       capture_output=True)


def main():
    os.chdir(r"C:\CustomOS\tobyOS")
    runs = int(os.environ.get("RUNS", "3"))
    results = []
    for i in range(1, runs + 1):
        results.append(one_run(i))
    print("=== qfreeze results: %s ===" % ", ".join(results), flush=True)


if __name__ == "__main__":
    main()
