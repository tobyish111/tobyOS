# intr-tsh.py -- ^C at the HOSTED shell, /bin/tsh interactive on a pty (an
# SSH session with -tt; the desktop's terminal window is the same shell on a
# pty of its own): batch 20's cases (tools/agent/probes/b20/kintr.py), with
# the answers interactive bash gives on a pty (b20/intr-bash.py, WSL).
#
# Usage: python intr-tsh.py HOST PORT
import os, re, subprocess, sys, time, threading
host, port = sys.argv[1], sys.argv[2]

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
NEVER = ["after-i3", "after-i4", "partial-i5", "trapped-i7", "it1", "after-i10",
         "after-i11"]

env = dict(os.environ)
env.setdefault("ProgramData", r"C:\ProgramData")
ssh = r"C:\Windows\System32\OpenSSH\ssh.exe"
p = subprocess.Popen([ssh, "-tt", "-o", "StrictHostKeyChecking=no", "-o",
                      "UserKnownHostsFile=NUL", "-o", "LogLevel=ERROR",
                      "-p", port, "root@" + host],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, env=env)
buf = []
lock = threading.Lock()
def reader():
    while True:
        d = p.stdout.read1(4096)
        if not d: break
        with lock: buf.append(d)
threading.Thread(target=reader, daemon=True).start()
def text():
    with lock: b = b"".join(buf)
    t = b.decode("utf-8", "replace")
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", t).replace("\r", "")
def send(s):
    p.stdin.write(s.encode()); p.stdin.flush()
def out_lines():
    # the shell's own lines, not the echo of what was typed
    return [l.strip() for l in text().split("\n")
            if l.strip() and "echo " not in l and "$?" not in l
            and not l.strip().startswith("tsh-") and not l.strip().startswith(">")]

time.sleep(4.0)
bad = 0
for name, pre, wait, post, marker, want in CASES:
    start = len(out_lines())
    send(pre); time.sleep(wait)
    send("\x03"); time.sleep(0.8)
    send(post)
    got = None
    end = time.time() + 12
    while time.time() < end and got is None:
        for l in out_lines()[start:]:
            if marker in l: got = l; break
        time.sleep(0.2)
    ok = got is not None and got.endswith(want.split(marker, 1)[1]) and marker in got
    print("%s %-4s want %-18r got %r" % ("ok " if ok else "BAD", name, want, got))
    if not ok: bad += 1
seen = [w for w in NEVER if any(w in l for l in out_lines())]
print("never printed:", "none" if not seen else seen)
send("exit\n"); time.sleep(1.0)
try: p.kill()
except Exception: pass
print("INTRTSH: " + ("ALL AS BASH" if not bad and not seen else "%d BAD, never-seen %s" % (bad, seen)))
sys.exit(1 if bad or seen else 0)
