# rawintr.py -- the raw bytes an interactive shell on a pty (ssh -tt) sends
# around one ^C: the text before it, the key, the line after. For telling
# apart where a newline goes after "^C" -- the line discipline's echo, the
# shell's own newline -- which intr-pty.py's line view cannot show.
#
# Usage: python rawintr.py HOST PORT tsh|bash 'LINE' ['AFTER']
import os, subprocess, sys, time, threading
host, port, shell, line = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
after = sys.argv[5] if len(sys.argv) > 5 else 'echo "st=$?"'
env = dict(os.environ)
env.setdefault("ProgramData", r"C:\ProgramData")
p = subprocess.Popen([r"C:\Windows\System32\OpenSSH\ssh.exe", "-tt", "-o",
                      "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=NUL",
                      "-o", "LogLevel=ERROR", "-p", port, "root@" + host],
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
def take(s):
    time.sleep(s)
    with lock:
        b = b"".join(buf); buf.clear()
    return b
def send(s):
    p.stdin.write(s.encode()); p.stdin.flush()
take(4.0)
if shell == "bash":
    send("exec /bin/bash --norc --noprofile -i\n"); take(3.0)
send("PS1='P$ '\n"); take(1.0)
# a LINE ending in \c is typed and left unfinished (^C at the prompt)
send(line[:-2] if line.endswith("\\c") else line + "\n"); a = take(1.5)
send("\x03"); b = take(1.5)
send(after + "\n"); c = take(1.5)
send("exit\n"); take(0.5)
try: p.kill()
except Exception: pass
print("line :", repr(a))
print("^C   :", repr(b))
print("after:", repr(c))
