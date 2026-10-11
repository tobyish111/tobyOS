# kintr.py -- ^C at the KERNEL shell, over TCP 7777 (the 0x03 byte a raw
# client sends). The cases and the answers wanted are interactive bash's on
# a pty (tools/agent/probes/b20/intr-bash.py): $? 130 everywhere, the rest of
# an interrupted line or loop dropped, a trapped INT letting the line go on,
# typeahead after the ^C kept. Then, over SSH, that nothing the ^C reached
# is still running. One case at a time, each waiting for its own marker, so
# a wedge names the case.
#
# Usage: python tools/agent/probes/b20/kintr.py HOST [PORT]
#        (ED_HOST / ED_PORT for the SSH half, as logs/edremote.sh takes them)
import os, socket, subprocess, sys, time
host = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 7777

CASES = [
    # (name, text before ^C, seconds before ^C, text after ^C, marker, want)
    ("i1", "sleep 30\n", 1.0, 'echo "i1 st=$?"\n', "i1 st=", "i1 st=130"),
    ("i2", "while :; do :; done\n", 1.0, 'echo "i2 st=$?"\n', "i2 st=", "i2 st=130"),
    ("i3", "sleep 30; echo after-i3\n", 1.0, 'echo "i3 st=$?"\n', "i3 st=", "i3 st=130"),
    ("i4", "rm -f /tmp/kif; mkfifo /tmp/kif; read x < /tmp/kif; echo after-i4\n", 1.5,
     'echo "i4 st=$?"\n', "i4 st=", "i4 st=130"),
    ("i5", "echo partial-i5", 0.5, 'echo "i5 st=$?"\n', "i5 st=", "i5 st=130"),
    ("i6", "if true\n", 0.5, 'echo "i6 st=$?"\n', "i6 st=", "i6 st=130"),
    ("i7", "trap 'echo trapped-i7' INT; sleep 30; echo after-i7 st=$?\n", 1.0,
     "trap - INT\n", "after-i7", "after-i7 st=130"),
    ("i8", "sleep 30\n", 1.0, 'echo "ahead-i8 st=$?"\n', "ahead-i8", "ahead-i8 st=130"),
    ("i10", "for i in 1 2 3; do sleep 30; echo it$i; done; echo after-i10\n", 1.0,
     'echo "i10 st=$?"\n', "i10 st=", "i10 st=130"),
    ("i11", "x=0; while [ $x -lt 3 ]; do sleep 30; x=$((x+1)); done; echo after-i11\n", 1.0,
     'echo "i11 st=$? x=$x"\n', "i11 st=", "i11 st=130 x=0"),
]
# what bash never prints in these cases
NEVER = ["after-i3", "after-i4", "partial-i5", "trapped-i7", "it1", "after-i10",
         "after-i11", "limit reached"]

s = socket.create_connection((host, port), timeout=10)
time.sleep(1.0)
s.settimeout(0.3)
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
        if until and until in out_lines(): return True
    return until is None
def out_lines():
    # the shell's own lines: not the echo of what was typed
    res = []
    for l in buf.replace("\r", "").split("\n"):
        l = l.strip()
        if l.startswith("tobyOS>") or l.startswith(">") or "echo " in l or "$?" in l:
            continue
        res.append(l)
    return "\n".join(res)

drain(None, 1.0)
bad = 0
for name, before, wait, after, marker, want in CASES:
    t0 = time.time()
    s.sendall(before.encode())
    drain(None, wait)
    t1 = time.time()
    s.sendall(b"\x03")
    s.sendall(after.encode())
    ok = drain(marker, 30)
    got = ""
    for l in out_lines().split("\n"):
        if marker in l:
            got = l
    print("%s %-4s %.1fs after ^C  %s" % ("ok " if ok and got == want else "BAD",
          name, time.time() - t1, got or "(nothing)"), flush=True)
    if not ok:
        print("KINTR: WEDGED AT " + name)
        sys.exit(1)
    if got != want:
        print("     want %r" % want)
        bad = 1
s.sendall(b"rm -f /tmp/kif; echo cleanup-done\n")   # the shell is shared
drain("cleanup-done", 10)
lines = out_lines()
for w in NEVER:
    if w in lines:
        print("BAD  printed %r, which bash never does here" % w)
        bad = 1
# nothing the ^C reached runs on
try:
    r = subprocess.run(["bash", "logs/edremote.sh",
                        "ps -ef | grep 'sleep 30' | grep -v grep | wc -l"],
                       capture_output=True, timeout=120, env=dict(os.environ))
    left = r.stdout.decode().replace("\r", "").strip().split("\n")[-1].strip()
except Exception as e:
    left = "ssh failed: %s" % e
print("%s leftover sleeps: %s" % ("ok " if left == "0" else "BAD", left))
if left != "0": bad = 1
print("KINTR: " + ("ALL AS BASH" if not bad else "MISMATCH"))
sys.exit(bad)
