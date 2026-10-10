# kfifo.py -- the KERNEL shell redirects to and from a FIFO, as bash does.
#
# It runs in pid 0, so it refused a FIFO (finding 96; probes/b16/kfifo.py
# asserted the refusal): the open waits for the other end and the reads and
# writes for data and room. pid 0 now waits as it waits for a foreground
# command -- its duties between looks of at most 10 ms (pipe.c pipe_park).
# Each step below has its other end: a background job of the kernel shell
# (a /bin/tsh child or a program), the same descriptor (`<>`), or an SSH
# session. k8 leaves the kernel shell WAITING on a reader's open, shows an
# SSH command answering meanwhile, and ends the wait from SSH. The values
# wanted are bash's (tools/agent/probes/b19/kfifo-steps.sh runs the same
# steps under the guest's bash). One command at a time, each waiting for its
# own marker, so a wedge names the step.
#
# Usage: python tools/agent/probes/b19/kfifo.py HOST [PORT]
#        (ED_HOST / ED_PORT for the SSH half, as logs/edremote.sh takes them)
import os, socket, subprocess, sys, time
host = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 7777
steps = [
    ("k1", "rm -f /tmp/kf /tmp/kf.out; mkfifo /tmp/kf; echo k1-done mk=$?"),
    ("k2", 'test -p /tmp/kf; echo "k2-done p=$?"'),
    ("k3a", "echo hello > /tmp/kf & echo k3a-done"),
    ("k3", 'read v < /tmp/kf; echo "k3-done st=$? v=$v"'),
    ("k4a", "/bin/cat /tmp/kf > /tmp/kf.out & echo k4a-done"),
    ("k4", 'echo world > /tmp/kf; echo "k4-done st=$?"'),
    ("k4b", 'sleep 1; echo "k4b-done out=$(cat /tmp/kf.out)"'),
    ("k5", 'exec 3<>/tmp/kf; echo both >&3; read w <&3; exec 3<&-; echo "k5-done w=$w"'),
    ("k6a", "printf 'a\\nbb\\n' > /tmp/kf & echo k6a-done"),
    ("k6", 'n=$(wc -c < /tmp/kf); echo "k6-done n=$((n))"'),
    ("k7a", "printf 'x\\ny\\nz\\n' > /tmp/kf & echo k7a-done"),
    ("k7", 'c=0; while read l; do c=$((c+1)); done < /tmp/kf; echo "k7-done c=$c"'),
    ("k8", 'read late < /tmp/kf; echo "k8-done late=$late"'),
    ("k9", 'rm -f /tmp/kf /tmp/kf.out; echo "k9-done st=$?"'),
]
want = {"k1": "mk=0", "k2": "p=0", "k3": "st=0 v=hello", "k4": "st=0",
        "k4b": "out=world", "k5": "w=both", "k6": "n=5", "k7": "c=3",
        "k8": "late=late", "k9": "st=0"}

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
def done_line(name):
    for l in reversed(buf.splitlines()):
        if (name + "-done") in l and "echo" not in l:
            return l.strip()
    return ""
def ssh(cmd, secs=120):
    t0 = time.time()
    try:
        r = subprocess.run(["bash", "logs/edremote.sh", cmd], capture_output=True,
                           timeout=secs, env=dict(os.environ))
        out = r.stdout.decode("utf-8", "replace").replace("\r", "").strip()
    except Exception as e:
        out = "ssh failed: %s" % e
    return out, time.time() - t0

drain("\x00", 1.0)
bad = 0
for name, cmd in steps:
    t0 = time.time()
    mark = len(buf)
    s.sendall((cmd + "\n").encode())
    if name == "k8":
        # the kernel shell must now be waiting for a writer
        early = drain("k8-done", 4)
        print("%s k8  waiting: %s" % ("ok " if not early else "BAD",
              "no answer yet" if not early else "answered without a writer"), flush=True)
        if early: bad = 1
        out, dt = ssh("echo answered-$((6*7))")
        okssh = "answered-42" in out
        print("%s k8  ssh while the kernel shell waits: %.1fs %r" %
              ("ok " if okssh and dt < 30 else "BAD", dt, out[-40:]), flush=True)
        if not okssh or dt >= 30: bad = 1
        out, dt = ssh("echo late > /tmp/kf; echo wrote=$?")
        print("     k8  ssh writer: %.1fs %r" % (dt, out[-20:]), flush=True)
    ok = drain(name + "-done", 30)
    print("%s %-4s %.1fs  %s" % ("ok " if ok else "HUNG", name, time.time() - t0,
          done_line(name) or cmd), flush=True)
    if not ok:
        print("KFIFO: WEDGED AT " + name)
        sys.exit(1)
for name, w in want.items():
    got = done_line(name)
    if w not in got:
        print("want %s %s, got %r" % (name, w, got))
        bad = 1
if "cannot open a FIFO" in buf:
    print("the refusal is still there")
    bad = 1
print("KFIFO: " + ("ALL AS WANTED" if not bad else "MISMATCH"))
sys.exit(bad)
