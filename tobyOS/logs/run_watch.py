#!/usr/bin/env python3
"""Slice 49: chromewin -> youtube.com/embed/<id> (lighter real MSE player than the
watch page). WHPX, 300s, watch for frames / crash / bootstrap; screendumps."""
import json, os, socket, subprocess, time

QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
ISO = "tobyOS.iso"
DISK = "disk.img"
SERIAL = "logs/run_watch.log"
PORT = 4464
# Slice 58: shots moved into the PLAYING window (~30-60s guest = ~67-97s
# runner; run-14's fixed times straddled it). Two shots 8s apart during
# playback = moving-frame proof; one late shot for the end state.
# Slice 61: e/f added -- the new choreography DWELLS at the comment section
# from ~guest 70s and only returns to the top at guest 280s, so mid-run shots
# show the comments/sidebar region (the visual proof this arc needs) and the
# final shot shows the player again after the return-to-top.
# Slice 61f: g/h added inside the PARK window (chromewin parks the rendered
# comment threads on screen from ~guest 170s and holds; runner time runs
# ~35-40s ahead of guest). Four chances at a comments-visible frame.
SHOTS = [(72, "logs/wat_a.png"), (80, "logs/wat_b.png"),
         (88, "logs/wat_c.png"), (150, "logs/wat_d.png"),
         (240, "logs/wat_e.png"), (270, "logs/wat_g.png"),
         (305, "logs/wat_h.png"), (330, "logs/wat_f.png")]
TOTAL = 360
# Slice 106: RUNSECS shortens the run for DIAGNOSTIC cycles. Chrome's GPU init
# either works or fails in the first ~10s, so an 8-minute run to read one error
# line is 7 minutes of nothing. Perf MEASUREMENT runs must stay at the default
# 360 -- every frame baseline in this arc is frames/360s.
TOTAL = int(os.environ.get("RUNSECS", TOTAL))


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

    # Slice 61c: snapshot=on -- disk.img is now a 1 GiB blank-partition GPT
    # image the kernel auto-provisions at boot; the snapshot overlay discards
    # all writes at exit, so EVERY run gets a pristine full-size /data (no
    # cross-run profile/cache contamination, no volume-fill carryover).
    # Slice 61d: WARM=1 disables the overlay for ONE run so chrome's profile
    # + caches (player JS, service worker) PERSIST onto disk.img; subsequent
    # snapshot runs then all start from that identical WARM image. A fully
    # cold profile every boot made page builds slow/flaky (ytd=54 skeleton
    # runs); a warm one is both faster and still deterministic across a batch.
    snap = ",snapshot=on" if os.environ.get("WARM") != "1" else ""
    # Slice 65: /data moves off ATA PIO onto virtio-blk. MEASURED (slice
    # 64c): pwrite64+openat+unlink were ~90% of ALL BKL hold time and the
    # lock was held 94% of wall clock -- because blk_ata.c is a PIO driver
    # (rep outsw per sector) and under WHPX every port access is a VM EXIT,
    # so one journalled write costs milliseconds with the global lock held.
    # tobyOS already registers a modern virtio-blk-pci driver at boot
    # (kernel.c virtio_blk_register) and the /data mount sweep finds the
    # tobyOS-data GPT partition on ANY block device, so this is a pure
    # transport swap: DMA rings instead of port-by-port PIO.
    # IDE_DATA=1 restores the old attachment for A/B comparison.
    if os.environ.get("IDE_DATA") == "1":
        disk_args = ["-drive", "file=%s,format=raw,if=ide,index=0,media=disk%s"
                     % (DISK, snap)]
    else:
        disk_args = ["-drive", "file=%s,format=raw,if=none,id=hd0%s" % (DISK, snap),
                     "-device", "virtio-blk-pci,drive=hd0"]
    cmd = [QEMU, "-cdrom", ISO] + disk_args + [
           "-boot", "d",
           "-netdev", "user,id=net0",
           "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
           "-accel", "whpx,kernel-irqchip=off",
           # Slice 88: -smp 4 was tried once the wake-handoff landed and
           # froze at 7.6s: a dozen threads READY in clone/clone3 for 232s
           # with ZERO BKL acquisitions on cpu1-3 -- the APs never ran the
           # work; likely the slice-87 quiesce/deferred-requeue interacting
           # with AP queues. Slice 89 chases that arc: SMP=4 env knob to
           # flip this run to 4 vCPUs (default stays the supported 1-vCPU
           # tier-2.5 config: wake handoff + idle-path sweeps).
           "-smp", os.environ.get("SMP", "1"),
           "-m", "8192", "-cpu", "qemu64,+smep,+smap",
           "-serial", "file:" + SERIAL,
           "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
           "-no-reboot"]
    # TIER 3 PHASE 1d (slice 106): GLDEV=1 attaches the 3D device so chrome can
    # raster on the host GPU, for the measure-first A/B against CPU raster.
    # Three things are load-bearing here and all three were paid for earlier:
    #   * virtio-gpu-gl-pci is ADDITIVE -- the std VGA stays and keeps doing
    #     scanout. Slice 96c proved SET_SCANOUT is REJECTED on the -gl device
    #     (QEMU routes everything there through virglrenderer), and slice 97
    #     bound it in a 3D-ONLY role for exactly that reason.
    #   * -display egl-headless, not none: virglrenderer needs a host GL
    #     context, and with -display none QEMU has none to give.
    #   * logs/angle on PATH: QEMU-for-Windows ships no EGL/GLES, which is what
    #     made egl-headless segfault until slice 96b staged Edge's ANGLE DLLs.
    env = None
    if os.environ.get("GLDEV") == "1":
        angle = os.path.join(os.getcwd(), "logs", "angle")
        if not os.path.isdir(angle):
            raise SystemExit("GLDEV=1 but logs/angle is missing -- "
                             "run `bash logs/setup_angle.sh` first")
        cmd += ["-device", "virtio-gpu-gl-pci", "-display", "egl-headless"]
        env = dict(os.environ, PATH=angle + os.pathsep + os.environ.get("PATH", ""))
        print("GLDEV=1: virtio-gpu-gl-pci + egl-headless, ANGLE from " + angle,
              flush=True)
    else:
        cmd += ["-display", "none"]
    print("launching qemu (WHPX)", flush=True)
    q = subprocess.Popen(cmd, stderr=subprocess.PIPE, env=env)
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
        seen = set()
        marks = ("[chromewin] chrome pid", "[chromewin] sessionId", "bootstrap OK",
                 "[chromewin] frame", "VIDEO ERROR", "EXCEPTION 14", "exit code=-1",
                 "terminating user process")
        while time.time() - start < TOTAL:
            if q.poll() is not None:
                print("QEMU exited early rc=%d" % q.returncode, flush=True)
                break
            try:
                txt = open(SERIAL, errors="replace").read()
            except OSError:
                txt = ""
            for mark in marks:
                cnt = txt.count(mark)
                key = (mark, cnt if mark == "[chromewin] frame" else 1)
                if cnt and key not in seen:
                    seen.add(key)
                    print("%6.1fs marker(%d): %s" % (time.time() - start, cnt, mark),
                          flush=True)
            if shot_i < len(SHOTS) and time.time() - start >= SHOTS[shot_i][0]:
                out = SHOTS[shot_i][1].replace("\\", "/")
                qmp(sock, {"execute": "screendump",
                           "arguments": {"filename": out, "format": "png"}})
                print("%6.1fs screendump -> %s" % (time.time() - start, out),
                      flush=True)
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
