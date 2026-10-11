# lessintr-linux.py -- what Linux's less and more do with ^C and ^Z, on a
# pty, in an interactive bash (run in WSL: python3 lessintr-linux.py). The
# answer the tobyOS pager is held to.
import os, pty, re, select, sys, time

def still_running(b):
    # the pager has the screen: no prompt after the key, and not stopped
    t = b.decode("utf-8", "replace").replace("\r", "")
    return "PROMPT$ " not in t.split("\n")[-1] and "Stopped" not in t

def run(cmd, keys):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "vt100"; os.environ["PS1"] = "PROMPT$ "
        os.chdir("/tmp")         # nothing a pager writes lands in the tree
        os.execv("/bin/bash", ["bash", "--norc", "--noprofile", "-i"])
    out = b""
    def take(t):
        nonlocal out
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try: out += os.read(fd, 65536)
                except OSError: return
    take(1.0)
    os.write(fd, (cmd + "\n").encode()); take(1.5)
    for k, t in keys:
        out = b""
        os.write(fd, k); take(t)
    tail = out
    running = still_running(tail)
    # a pager still running after the key gets its q before anything else
    # is typed: typed into less, `echo ...` runs less commands (s saves a
    # file of the input under whatever name follows)
    if running:
        os.write(fd, b"q"); take(0.8)
    os.write(fd, b'echo "after st=$?"\n'); out = b""; take(1.5)
    after = out
    os.write(fd, b'exit\n'); take(0.5)
    try: os.kill(pid, 9)
    except Exception: pass
    vis = lambda b: re.sub(r"\x1b\[[0-9;?>=]*[A-Za-z]|\x1b[=>]", "", b.decode("utf-8", "replace")).replace("\r", "")
    return vis(tail)[-200:], running, vis(after)[-200:]

for cmd in ("seq 1 200 | less", "seq 1 200 | more", "seq 1 200 | less -K"):
    for name, keys in (("^C", [(b"\x03", 1.0)]), ("^Z", [(b"\x1a", 1.0)])):
        t, running, a = run(cmd, keys)
        print("== %s, %s" % (cmd, name))
        print("   after key :", repr(t[-120:]))
        print("   still paging after the key:", running, "(then q)" if running else "")
        print("   then echo :", repr(a[-120:]))
