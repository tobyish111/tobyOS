#!/usr/bin/env python3
"""Slice 91: freeze-catcher. The -smp 4 wedge is INTERMITTENT and its worst
shape is total serial silence (even the 3s sched_tick heartbeat stops), so
nothing in-guest can report it. But QEMU still answers QMP -- so run the
exact run_watch.py configuration, watch the serial log for stall, and when
the guest goes quiet capture `info registers -a` for every vCPU, twice, a
few seconds apart. Two identical RIP sets = halted/deadlocked; differing =
live spin. addr2line -e tobyos.bin <rip> then names the exact line each CPU
is stuck on. (Same method that cracked the BKL double-acquire freeze.)

Usage:  SMP=4 python logs/run_freeze.py
Output: logs/freeze_regs.txt (capture) or "no freeze in N runs".
"""
import json, os, socket, subprocess, time

QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
ISO = "tobyOS.iso"
DISK = "disk.img"
SERIAL = "logs/run_watch.log"
PORT = 4465
RUNS = int(os.environ.get("RUNS", "4"))   # freeze is intermittent; try a few
RUN_SECS = 360
STALL_SECS = 25     # heartbeats come every 3s; deep dump bursts ~5s


class Qmp:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.f = self.sock.makefile("rwb")
        json.loads(self.f.readline())               # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        req = {"execute": name}
        if args:
            req["arguments"] = args
        self.f.write((json.dumps(req) + "\r\n").encode())
        self.f.flush()
        while True:                                  # skip async events
            line = self.f.readline()
            if not line:
                return None
            msg = json.loads(line)
            if "return" in msg or "error" in msg:
                return msg

    def hmp(self, cl):
        r = self.cmd("human-monitor-command", **{"command-line": cl})
        return r.get("return", "") if r else ""


def one_run(run_idx):
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
    print("run %d: launching (smp=%s)" % (run_idx, smp), flush=True)
    q = subprocess.Popen(cmd, stderr=subprocess.PIPE)
    qmp = None
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
        armed = False               # only meaningful after the first [hb
        last_size = -1
        last_growth = time.time()
        while time.time() - start < RUN_SECS:
            time.sleep(2)
            if q.poll() is not None:
                print("QEMU exited early", flush=True)
                return False
            blob = b""
            try:
                blob = open(SERIAL, "rb").read()
                size = len(blob)
                if not armed and b"[hb " in blob:
                    armed = True
                    print("run %d: armed (kernel heartbeat seen)" % run_idx,
                          flush=True)
            except OSError:
                size = -1
            # Slice 92: the MILDER freeze shape -- serial still flowing
            # (heartbeats alive) but a deep dump shows ZERO BKL acquisitions
            # on EVERY cpu = the kernel is wedged while the tick narrates it.
            # The old growth-only detector sailed straight past this shape.
            if armed and size > 0:
                tail = blob[-8000:]
                zeros = sum(1 for c in (b"cpu0", b"cpu1", b"cpu2", b"cpu3")
                            if (b"[bkl] " + c + b" acq=0 ") in tail)
                if zeros >= int(os.environ.get("SMP", "4")):
                    print("run %d: WEDGED-ALIVE (all-CPU acq=0 in deep dump)"
                          " -- capturing" % run_idx, flush=True)
                    with open("logs/freeze_regs.txt", "w") as out:
                        out.write("=== wedged-alive capture run %d ===\n"
                                  % run_idx)
                        out.write("=== capture 1 ===\n")
                        out.write(qmp.hmp("info registers -a"))
                        time.sleep(3)
                        out.write("\n=== capture 2 (3s later) ===\n")
                        out.write(qmp.hmp("info registers -a"))
                    import shutil
                    shutil.copyfile(SERIAL, "logs/freeze_run%d.log" % run_idx)
                    return True
            if size != last_size:
                last_size = size
                last_growth = time.time()
                continue
            if armed and time.time() - last_growth > STALL_SECS:
                guest_ms = "?"
                try:
                    tail = open(SERIAL, "rb").read()[-4000:]
                    i = tail.rfind(b" ms]")
                    j = tail.rfind(b"[", 0, i)
                    guest_ms = tail[j + 1:i].decode(errors="replace")
                except Exception:
                    pass
                print("run %d: FROZEN (no serial %ds, last guest ts %s ms) "
                      "-- capturing" % (run_idx, STALL_SECS, guest_ms),
                      flush=True)
                with open("logs/freeze_regs.txt", "w") as out:
                    out.write("=== freeze capture run %d, last guest ts %s "
                              "ms ===\n" % (run_idx, guest_ms))
                    out.write("=== capture 1 ===\n")
                    out.write(qmp.hmp("info registers -a"))
                    time.sleep(3)
                    out.write("\n=== capture 2 (3s later; same RIPs = "
                              "halted/deadlock, moving = spin) ===\n")
                    out.write(qmp.hmp("info registers -a"))
                    out.write("\n=== info cpus ===\n")
                    out.write(qmp.hmp("info cpus"))
                print("run %d: captured -> logs/freeze_regs.txt" % run_idx,
                      flush=True)
                import shutil
                shutil.copyfile(SERIAL, "logs/freeze_run%d.log" % run_idx)
                return True
        print("run %d: completed %ds without freezing" % (run_idx, RUN_SECS),
              flush=True)
        # Slice 92: keep every run's serial log -- the next run deletes
        # SERIAL, and more than one "clean" run turned out to carry evidence.
        try:
            import shutil
            shutil.copyfile(SERIAL, "logs/freeze_run%d.log" % run_idx)
        except OSError:
            pass
        return False
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
    for i in range(1, RUNS + 1):
        if one_run(i):
            return
    print("no freeze captured in %d runs" % RUNS, flush=True)


if __name__ == "__main__":
    main()
