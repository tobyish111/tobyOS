"""Slice 119: boot the desktop and CLICK the taskbar's Chromium pin over QMP.

Pin geometry mirrors gui.c exactly (change one, change the other):
    x0     = START_BTN_W(62) + 4 + TASKBAR_SEARCH_W(160) + 8      = 234
    pin i  = x0 + i * TASKBAR_PIN_W(44), usable width 44-6        = 38
    centre = pin_x + 19
    Chromium is the LAST pin, index 8                             -> x = 605
    taskbar top = screen_h - GUI_TASKBAR_H(44); centre            -> y = 778

The mouse is PS/2 (relative). To land on an absolute point we slam the
cursor into the corner first -- the driver clamps there, giving a known
(0,0) origin -- then move by the target delta. Screendumps bracket the
click so the cursor's real landing spot is visible if the arithmetic is off.
"""
import json, os, socket, subprocess, sys, time

QEMU  = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
ISO   = r"C:\CustomOS\tobyOS\tobyOS.iso"
DISK  = r"C:\CustomOS\tobyOS\disk.img"
SER   = r"C:\CustomOS\tobyOS\logs\pintest.log"
PORT  = 4459
SCR_W, SCR_H = 1280, 800
PIN_X, PIN_Y = 234 + 8 * 44 + 19, SCR_H - 44 // 2      # -> (605, 778)


def qmp(sock, obj, quiet=True):
    """Send a QMP command and RETURN the reply. The first cut of this script
    discarded replies, so a rejected input-send-event looked identical to a
    delivered one -- the guest simply never moved and nothing said why."""
    sock.sendall((json.dumps(obj) + "\n").encode())
    time.sleep(0.25)
    try:
        raw = sock.recv(65536).decode(errors="replace")
    except Exception as e:
        raw = "<no reply: %s>" % e
    if not quiet or '"error"' in raw:
        print("   QMP %s -> %s" % (obj.get("execute"), raw.strip()[:300]),
              flush=True)
    return raw


def ev(sock, events, quiet=False):
    return qmp(sock, {"execute": "input-send-event",
                      "arguments": {"events": events}}, quiet=quiet)


def rel(axis, value):
    return {"type": "rel", "data": {"axis": axis, "value": value}}


def shot(sock, name):
    qmp(sock, {"execute": "screendump",
               "arguments": {"filename": name.replace("\\", "/"), "format": "png"}})
    print("   screendump -> %s" % name, flush=True)


def main():
    if os.path.exists(SER):
        os.remove(SER)
    q = subprocess.Popen([QEMU, "-cdrom", ISO,
                          "-drive", "file=%s,format=raw,if=none,id=hd0,snapshot=on" % DISK,
                          "-device", "virtio-blk-pci,drive=hd0",
                          "-boot", "d",
                          "-netdev", "user,id=net0",
                          "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
                          "-accel", "whpx,kernel-irqchip=off",
                          "-smp", "4", "-m", "8192", "-cpu", "qemu64,+smep,+smap",
                          "-serial", "file:" + SER,
                          "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT,
                          "-display", "none", "-no-reboot"])
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
        buf = b""
        while not buf.endswith(b"\n"):
            buf += sock.recv(1)
        qmp(sock, {"execute": "qmp_capabilities"})

        # Wait for the desktop. The TKAPP harness auto-logs-in and launches
        # gui_about; its window appearing means the shell is pumping input.
        print("waiting for the desktop...", flush=True)
        deadline = time.time() + 180
        while time.time() < deadline:
            try:
                txt = open(SER, errors="replace").read()
            except OSError:
                txt = ""
            if "[TKAPP] launching" in txt or "gui_about" in txt:
                print("   desktop up at %.0fs" % (180 - (deadline - time.time())),
                      flush=True)
                break
            time.sleep(2)
        time.sleep(20)                      # let the desktop settle/paint
        shot(sock, r"C:\CustomOS\tobyOS\logs\pin_before.png")

        # The click itself is synthesised INSIDE the guest by the TKAPP
        # harness (gui_post_shell_click) -- host injection is a dead end
        # here: QEMU accepts relative input-send-event and returns success,
        # but nothing reaches IRQ12 and the cursor never moves. Kept as a
        # one-line probe so a future QEMU/driver change shows up as the
        # cursor MOVING in pin_hover.png rather than being rediscovered.
        print("probing host input (expected: no cursor movement)...", flush=True)
        ev(sock, [rel("x", -4000), rel("y", -4000)])
        time.sleep(1)
        ev(sock, [rel("x", PIN_X), rel("y", PIN_Y)])
        time.sleep(2)
        shot(sock, r"C:\CustomOS\tobyOS\logs\pin_hover.png")

        # Chrome needs a while to bootstrap under WHPX; shoot along the way.
        for i, wait in enumerate((20, 40, 60, 60)):
            time.sleep(wait)
            shot(sock, r"C:\CustomOS\tobyOS\logs\pin_after%d.png" % (i + 1))
        qmp(sock, {"execute": "quit"})
    finally:
        try:
            q.wait(timeout=10)
        except Exception:
            q.kill()


main()
