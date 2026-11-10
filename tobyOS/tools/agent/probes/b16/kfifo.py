# kfifo.py -- the KERNEL shell and a FIFO (finding 96). It runs on pid 0's
# poll loop, which must never wait, so it refuses to open one for a
# redirection -- opened as a file, a FIFO read as empty and kept what was
# written in the node. A program it starts opens the FIFO itself and
# waits in its own context, as under /bin/tsh. One command at a time, each
# waiting for its own marker, so a wedge names the step.
#
# Usage: python tools/agent/probes/b16/kfifo.py HOST [PORT]
import socket, sys, time
host = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 7777
steps = [
    ("k1", "rm -f /tmp/kf; mkfifo /tmp/kf; echo k1-done mk=$?"),
    ("k2", 'test -p /tmp/kf; echo "k2-done p=$?"'),
    ("k3", 'echo x > /tmp/kf; echo "k3-done st=$?"'),
    ("k4", 'read v < /tmp/kf; echo "k4-done st=$?"'),
    ("k5", 'x=$(/bin/ls -l /tmp/kf | cut -c1-10); echo "k5-done $x"'),
    ("k6", 'x=$(wc -c < /tmp/kf 2>&1); echo "k6-done st=$?"'),
    ("k7", 'rm /tmp/kf; echo "k7-done st=$?"'),
]
s = socket.create_connection((host, port), timeout=10)
time.sleep(1.0)
s.settimeout(0.5)
buf = ""
def drain(until, secs):
    global buf
    end = time.time() + secs
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d: return False
            buf += d.decode("utf-8", "replace")
        except Exception:
            pass
        if until in buf: return True
    return False
drain("\x00", 1.0)
bad = 0
for name, cmd in steps:
    t0 = time.time()
    mark = len(buf)
    s.sendall((cmd + "\n").encode())
    ok = drain(name + "-done", 20)
    line = ""
    for l in buf[mark:].splitlines():
        if (name + "-done") in l and "echo" not in l:
            line = l.strip()
    print("%s %-3s %.1fs  %s" % ("ok " if ok else "HUNG", name, time.time() - t0, line or cmd), flush=True)
    if not ok:
        print("KFIFO: WEDGED AT " + name)
        sys.exit(1)
want = {"k1": "mk=0", "k2": "p=0", "k3": "st=1", "k4": "st=1",
        "k5": "prw-r--r--", "k7": "st=0"}
for name, w in want.items():
    got = [l for l in buf.splitlines() if (name + "-done") in l and "echo" not in l]
    if not got or w not in got[-1]:
        print("want %s %s, got %r" % (name, w, got[-1] if got else None))
        bad = 1
print("KFIFO: " + ("ALL AS WANTED" if not bad else "MISMATCH"))
sys.exit(bad)
