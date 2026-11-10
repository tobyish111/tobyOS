# intrpty.py -- ^C at an interactive shell on a pty (ssh -tt, as the
# terminal window): `sleep 15; echo after-sleep`, ^C after 2 s. bash: the
# list is dropped (no after-sleep) and $? is 130.
# Usage: python intrpty.py HOST PORT
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
take(4.0)
send('echo "flags=$-"\n'); a = take(1.5)
send("sleep 15; echo after-sleep\n"); take(2.0)
send("\x03"); b = take(1.5)
send('echo "st=$?"\n'); c = take(1.5)
send("exit\n"); take(1.0)
try: p.kill()
except Exception: pass
v = lambda t: re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", t).replace("\r", "")
print("flags :", re.findall(r"flags=\S*", v(a))[-1:])
print("after ^C:", repr(v(b)[-80:]))
print("status :", re.findall(r"st=\d+", v(c))[-1:])
ok = "after-sleep" not in v(b) + v(c) and "st=130" in v(c)
print("INTRPTY: " + ("AS BASH" if ok else "NOT AS BASH"))
