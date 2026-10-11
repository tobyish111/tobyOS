# lesspty2.py -- less and more on a terminal, case by case (SSH on a pty,
# as the desktop's terminal window): a file, a pipe into more, a search, the
# arrow keys, ^C, an input that fits the screen, and less whose output is
# not a terminal (it copies its input through, as Linux's does). After each,
# a marker line shows the shell has the terminal back, cooked: its echo.
#
# Usage: python lesspty2.py HOST PORT
import os, re, subprocess, sys, time, threading
host, port = sys.argv[1], sys.argv[2]
env = dict(os.environ)
env.setdefault("ProgramData", r"C:\ProgramData")
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
        d = p.stdout.read1(4096)
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
def vis(t):
    t = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", " ", t)
    return re.sub(r"[ \r]+", " ", t)
def status(t):
    v = vis(t)
    m = re.findall(r"line (\d+)/(\d+)", v)
    if m:
        return m[-1]
    m = re.findall(r"\(lines (\d+)\) -- top", v)    # the first screen
    return ("1", m[-1]) if m else None

take(4.0)
send("seq 1 200 > /tmp/lp.txt; echo ready\n"); take(1.5)
res = {}
# 1. a file: keys from stdin, which IS the terminal
send("less /tmp/lp.txt\n"); a = take(2.5)
send(" "); b = take(1.5)
send("q"); take(1.0)
send('echo "m1 st=$?"\n'); c = take(1.5)
res["file"] = (status(a), status(b), "m1 st=0" in vis(c))
# 2. a pipe into more
send("seq 1 200 | more\n"); a = take(2.5)
send(" "); b = take(1.5)
send("q"); take(1.0)
send('echo "m2 st=$?"\n'); c = take(1.5)
res["more"] = (status(a), status(b), "m2 st=0" in vis(c))
# 3. search: /150 Enter
send("seq 1 200 | less\n"); take(2.5)
send("/150\r"); b = take(1.5)
send("q"); take(1.0)
send('echo "m3 st=$?"\n'); c = take(1.5)
res["search"] = (None, status(b), "m3 st=0" in vis(c))
# 4. arrow keys: down three
send("seq 1 200 | less\n"); take(2.5)
send("\x1b[B\x1b[B\x1b[B"); b = take(1.5)
send("q"); take(1.0)
send('echo "m4 st=$?"\n'); c = take(1.5)
res["arrows"] = (None, status(b), "m4 st=0" in vis(c))
# 5. ^C quits, and the terminal is cooked again (the echo comes back)
send("seq 1 200 | less\n"); take(2.5)
send("\x03"); take(1.0)
send('echo "m5 st=$?"\n'); c = take(1.5)
res["ctrlc"] = (None, None, "m5 st=0" in vis(c))
# 6. it fits the screen: printed, no pager
send("seq 1 5 | less; echo m6-done\n"); c = take(2.5)
res["fits"] = (None, None, "m6-done" in vis(c) and " 5 " in vis(c))
# 7. output not a terminal: copied through
send("seq 1 200 | less | tail -1; seq 1 200 | less > /tmp/lp2.txt; wc -l < /tmp/lp2.txt\n"); c = take(3.0)
res["notty"] = (None, None, " 200 " in vis(c))
send("rm -f /tmp/lp.txt /tmp/lp2.txt; exit\n"); take(1.0)
try: p.kill()
except Exception: pass

want = {
    "file":   lambda r: r[0] is not None and r[1] is not None and int(r[1][0]) > int(r[0][0]) and r[2],
    "more":   lambda r: r[0] is not None and r[1] is not None and int(r[1][0]) > int(r[0][0]) and r[2],
    "search": lambda r: r[1] is not None and int(r[1][0]) == 150 and r[2],
    "arrows": lambda r: r[1] is not None and int(r[1][0]) == 4 and r[2],
    "ctrlc":  lambda r: r[2],
    "fits":   lambda r: r[2],
    "notty":  lambda r: r[2],
}
bad = 0
for k in ("file", "more", "search", "arrows", "ctrlc", "fits", "notty"):
    ok = want[k](res[k])
    print("%s %-7s %s" % ("ok " if ok else "BAD", k, res[k]))
    if not ok: bad = 1
print("LESSPTY2: " + ("ALL AS WANTED" if not bad else "MISMATCH"))
sys.exit(bad)
