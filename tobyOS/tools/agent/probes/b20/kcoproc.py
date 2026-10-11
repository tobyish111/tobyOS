# kcoproc.py -- coprocesses in the KERNEL shell, over TCP 7777. The steps
# are tools/agent/probes/b20/kcoproc-steps.sh, one command line each; the
# lines wanted are the guest's bash's for that script (and /bin/tsh's, the
# same -- logs/b20 co2). The kernel shell said "coproc: not available in the
# kernel shell"; its coprocess is a /bin/tsh child now. Last, a ^C ends a
# `read` from a coprocess that never writes ($? 130, the coprocess runs on,
# then goes when its input is closed and it is waited for).
#
# Usage: python tools/agent/probes/b20/kcoproc.py HOST [PORT]
import os, socket, sys, time
host = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 7777
here = os.path.dirname(os.path.abspath(__file__))

WANT = [
    "c1 fds=63 60 pid=set",
    "c2 got=hello",
    'c3 declare -a COPROC=([N]="N" [N]="N")',
    "c4 [63 -1]",
    "c5 wait=0 [unset] [unset]",
    "c6 fds=63 60",
    "c7 named got ping",
    "c8 w2=0",
    "c9 [unset]",
    "c10 [63 60]",
    "c10b [63 -1]",
    "c10c [-1 -1]",
    "c10d [unset]",
    "c11 r=named got ping",
    "c11b [unset]",
    "c12 bad=1",
    "c13 st=130",
    "c14 [unset]",
]

steps = []
for l in open(os.path.join(here, "kcoproc-steps.sh")):
    l = l.rstrip("\n")
    if not l or l.startswith("#") or l.startswith("exec 2>&1"):
        continue
    steps.append(l)

s = socket.create_connection((host, port), timeout=10)
time.sleep(1.0)
s.settimeout(0.3)
buf = ""
def shell_lines():
    res = []
    for l in buf.replace("\r", "").split("\n"):
        l = l.strip()
        if l.startswith("tobyOS>") or l.startswith(">") or "echo " in l:
            continue
        res.append(l)
    return res
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
        if until and until in shell_lines(): return True
    return until is None
drain(None, 1.0)
for k, st in enumerate(steps):
    s.sendall((st + "\n").encode())
    s.sendall(('echo "sync-%d"\n' % k).encode())
    if not drain("sync-%d" % k, 40):
        print("KCOPROC: WEDGED AT step %d: %s" % (k, st))
        sys.exit(1)
# c13: a ^C ends a read from a coprocess that writes nothing
s.sendall(b'coproc cat; read -r z <&"${COPROC[0]}"; echo "c13 after-read"\n')
drain(None, 1.5)
s.sendall(b"\x03")
s.sendall(b'echo "c13 st=$?"\n')
if not drain("c13 st=130", 20):
    print("KCOPROC: the ^C did not end the read")
s.sendall(b'exec {COPROC[1]}>&-; wait $COPROC_PID; echo "c14 [${COPROC[@]-unset}]"\n')
drain("c14 [unset]", 20)

got = [l for l in shell_lines() if l[:1] == "c" and l[1:2].isdigit()]
bad = 0
for i in range(max(len(got), len(WANT))):
    g = got[i] if i < len(got) else "(none)"
    w = WANT[i] if i < len(WANT) else "(none)"
    ok = g == w
    print("%s %s" % ("ok " if ok else "BAD", g if ok else "%s   want %s" % (g, w)))
    if not ok: bad = 1
if "c13 after-read" in got: bad = 1
if "not available in the kernel shell" in buf:
    print("the refusal is still there")
    bad = 1
print("KCOPROC: " + ("ALL AS BASH" if not bad else "MISMATCH"))
sys.exit(bad)
