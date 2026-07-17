#!/usr/bin/env python3
"""Edge-as-oracle visual comparison for the tobyOS browser.

Boots the (already built, -DNAV_URL-baked) tobyOS.iso headless in QEMU with
SLIRP networking, waits for the page to settle, QMP-screendumps, and crops
the browser content area. Renders the same URL in headless Microsoft Edge at
the matching viewport (719x435). Emits a labeled side-by-side composite plus
a coarse structural-difference score (mean per-cell luminance delta on a
16x16 grid) so punch lists get numbers, not just eyeballs.

Run via compare.sh (which does the ISO build); direct use:
  python tools/compare/compare.py --url https://... --slug wiki --out C:\\path\\out

Caveats (by design):
  - Dynamic pages differ run to run (news rotates); compare structure, not
    content. Pixel-perfect diffing across engines is meaningless.
  - Edge sends its native UA (it is the oracle); tobyOS sends its own UA and
    may be served different HTML (e.g. youtube serves ES5). Note per-site.
  - Top-of-page only (no scrolling) in v1.
  - The intermittent "TKAPP stall" kills a boot at ~3s with no panic; the
    driver detects it (newest serial timestamp too old) and retries.
"""
import argparse, json, os, re, socket, subprocess, sys, time

QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

# Browser content area inside the 1280x800 tobyOS screendump.
CROP = (91, 110, 810, 545)                     # -> 719x435
VIEW_W, VIEW_H = CROP[2] - CROP[0], CROP[3] - CROP[1]


def qmp_cmd(sock_file, obj):
    sock_file.write(json.dumps(obj) + "\n")
    sock_file.flush()
    while True:
        line = sock_file.readline()
        if not line:
            raise RuntimeError("QMP closed")
        msg = json.loads(line)
        if "return" in msg or "error" in msg:
            return msg


def kill_qemu():
    subprocess.run(["taskkill", "/IM", "qemu-system-x86_64.exe", "/F"],
                   capture_output=True)


def serial_latest_ms(path):
    """Newest '[N ms]' timestamp in the (binary-ish) serial log, else -1."""
    try:
        data = open(path, "rb").read().decode("latin-1")
    except OSError:
        return -1
    stamps = re.findall(r"\[\s*(\d+) ms\]", data)
    return int(stamps[-1]) if stamps else -1


def boot_and_shot(iso, out_png, serial_log, wait_s, port):
    kill_qemu()
    time.sleep(1)
    for p in (out_png, serial_log):
        if os.path.exists(p):
            os.remove(p)
    cmd = [QEMU, "-cdrom", iso, "-boot", "d", "-smp", "1", "-m", "512",
           "-display", "none", "-netdev", "user,id=n0",
           "-device", "e1000,netdev=n0",
           "-serial", "file:" + serial_log,
           "-qmp", "tcp:127.0.0.1:%d,server,nowait" % port, "-no-reboot"]
    qemu = subprocess.Popen(cmd)
    try:
        time.sleep(wait_s)
        latest = serial_latest_ms(serial_log)
        if latest < 15000:                     # the ~3s TKAPP stall signature
            print("  stall detected (latest serial ts %d ms)" % latest,
                  flush=True)
            return False
        sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        f = sock.makefile("rw", encoding="utf-8", newline="\n")
        f.readline()                           # greeting
        qmp_cmd(f, {"execute": "qmp_capabilities"})
        r = qmp_cmd(f, {"execute": "screendump",
                        "arguments": {"filename": out_png, "format": "png"}})
        time.sleep(1)
        sock.close()
        if "error" in r:
            print("  screendump error:", r, flush=True)
            return False
        return os.path.exists(out_png) and os.path.getsize(out_png) > 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except Exception:
            kill_qemu()


def edge_shot(url, out_png, profile_dir):
    if os.path.exists(out_png):
        os.remove(out_png)
    # A dedicated profile keeps headless runs from colliding with the
    # user's real Edge session (a shared profile makes --screenshot
    # silently no-op when any other Edge instance holds it).
    subprocess.run([EDGE, "--headless", "--disable-gpu",
                    "--user-data-dir=" + profile_dir,
                    "--screenshot=" + out_png,
                    "--window-size=%d,%d" % (VIEW_W, VIEW_H),
                    "--hide-scrollbars", url],
                   capture_output=True, timeout=120)
    return os.path.exists(out_png) and os.path.getsize(out_png) > 0


def grid_score(img_a, img_b, n=16):
    """Mean |luminance delta| (0..255) on an n x n grid; plus the grid."""
    a = img_a.convert("L").resize((n, n))
    b = img_b.convert("L").resize((n, n))
    pa, pb = list(a.getdata()), list(b.getdata())
    cells = [abs(x - y) for x, y in zip(pa, pb)]
    grid = [cells[r * n:(r + 1) * n] for r in range(n)]
    return sum(cells) / len(cells), grid


def compose(toby, edge, slug, score, out_png):
    from PIL import Image, ImageDraw
    w = 640
    th = int(toby.height * w / toby.width)
    eh = int(edge.height * w / edge.width)
    t = toby.resize((w, th))
    e = edge.resize((w, eh))
    pad, label_h = 12, 26
    H = label_h + max(th, eh) + pad * 2
    canvas = Image.new("RGB", (w * 2 + pad * 3, H), (24, 26, 32))
    d = ImageDraw.Draw(canvas)
    d.text((pad, 6), "tobyOS  |  %s" % slug, fill=(240, 240, 240))
    d.text((w + pad * 2, 6), "Edge (oracle)  |  grid-diff %.1f" % score,
           fill=(240, 240, 240))
    canvas.paste(t, (pad, label_h))
    canvas.paste(e, (w + pad * 2, label_h))
    canvas.save(out_png)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--url", required=True)
    ap.add_argument("--slug", required=True)
    ap.add_argument("--iso", default=r"C:\CustomOS\tobyOS\tobyOS.iso")
    ap.add_argument("--out", required=True, help="output directory (C:\\ path)")
    ap.add_argument("--wait", type=int, default=70,
                    help="seconds for the page to settle in tobyOS")
    ap.add_argument("--port", type=int, default=4550)
    ap.add_argument("--retries", type=int, default=3)
    ap.add_argument("--skip-toby", action="store_true",
                    help="reuse an existing toby_<slug>.png (skip the boot)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    from PIL import Image

    toby_full = os.path.join(args.out, "toby_%s_full.png" % args.slug)
    toby_png = os.path.join(args.out, "toby_%s.png" % args.slug)
    edge_png = os.path.join(args.out, "edge_%s.png" % args.slug)
    serial_log = os.path.join(args.out, "serial_%s.log" % args.slug)
    comp_png = os.path.join(args.out, "compare_%s.png" % args.slug)

    if not args.skip_toby:
        ok = False
        for attempt in range(1, args.retries + 1):
            print("tobyOS boot attempt %d/%d ..." % (attempt, args.retries),
                  flush=True)
            if boot_and_shot(args.iso, toby_full, serial_log, args.wait,
                             args.port):
                ok = True
                break
        if not ok:
            sys.exit("tobyOS screenshot failed after retries")
        Image.open(toby_full).crop(CROP).save(toby_png)

    print("edge shot ...", flush=True)
    profile_dir = os.path.join(args.out, "edge_profile")
    if not edge_shot(args.url, edge_png, profile_dir):
        sys.exit("edge screenshot failed")

    toby = Image.open(toby_png).convert("RGB")
    edge = Image.open(edge_png).convert("RGB")
    score, grid = grid_score(toby, edge)
    compose(toby, edge, args.slug, score, comp_png)

    with open(os.path.join(args.out, "score_%s.txt" % args.slug), "w") as f:
        f.write("url: %s\ngrid-diff (mean |dL| 16x16, 0-255): %.2f\n\n"
                % (args.url, score))
        f.write("cell heat (0-9 = delta/28):\n")
        for row in grid:
            f.write("".join(str(min(9, c // 28)) for c in row) + "\n")
    print("grid-diff %.2f -> %s" % (score, comp_png), flush=True)


if __name__ == "__main__":
    main()
