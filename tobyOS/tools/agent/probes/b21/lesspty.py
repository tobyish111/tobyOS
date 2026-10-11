# lesspty.py -- `less` and `more` on a TERMINAL, as the desktop's terminal
# window runs them: an SSH session on a pty (ssh -tt), /bin/tsh in it,
# `seq 1 100 | less`, then keys. A pager whose input is a pipe must read
# its keys from the terminal; one that reads them from standard input
# finds the pipe at its end and quits after the first screen.
#
# Usage: python lesspty.py HOST PORT
#   prints what each step showed; LESSPTY: PAGED or LESSPTY: NOT PAGED
import os, re, subprocess, sys, time, threading
host, port = sys.argv[1], sys.argv[2]
env = dict(os.environ)
env.setdefault("ProgramData", r"C:\ProgramData")    # see logs/winenv.sh
ssh = r"C:\Windows\System32\OpenSSH\ssh.exe"
p = subprocess.Popen([ssh, "-tt", "-o", "StrictHostKeyChecking=no", "-o",
                      "UserKnownHostsFile=NUL", "-o", "LogLevel=ERROR",
                      "-p", port, "root@" + host],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, env=env)
out = []
lock = threading.Lock()
def reader():
    while True:
        d = p.stdout.read1(4096) if hasattr(p.stdout, "read1") else p.stdout.read(1)
        if not d: break
        with lock: out.append(d)
threading.Thread(target=reader, daemon=True).start()
def take(secs):
    time.sleep(secs)
    with lock:
        b = b"".join(out); out.clear()
    return b.decode("utf-8", "replace")
def send(s):
    p.stdin.write(s.encode()); p.stdin.flush()
def visible(t):
    t = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", " ", t)
    return re.sub(r"[ \r]+", " ", t)

take(4.0)
send("seq 1 100 | less\n")
a = take(3.0)
send(" ")
b = take(2.0)
send("q")
c = take(2.0)
send('echo "after-less st=$?"\n')
d = take(2.0)
send("exit\n")
take(1.0)
try: p.kill()
except Exception: pass
first = re.findall(r"\b(\d+)\b", visible(a))
second = re.findall(r"\b(\d+)\b", visible(b))
print("first screen: lines %s..%s" % (first[2] if len(first) > 2 else "?", first[-1] if first else "?"))
print("after space : %s" % (" ".join(second[:3]) + " ... " + " ".join(second[-3:]) if second else "(nothing)"))
print("after q     : %r" % visible(c)[-60:])
print("echo        : %r" % visible(d)[-60:])
paged = bool(second) and "after-less st=0" in visible(d) and \
        any(int(x) > 30 for x in second if x.isdigit() and int(x) <= 100)
print("LESSPTY: " + ("PAGED" if paged else "NOT PAGED"))
