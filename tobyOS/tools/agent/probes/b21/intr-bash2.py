# intr-bash2.py -- the ORACLE for ^C at an interactive shell whose foreground
# job dies of it (bash's wait_for under job control: the shell acts as if
# it had the SIGINT in a loop or a list, and not otherwise). Interactive
# bash on a pty, only the marker lines kept. Run where bash and a pty are
# (WSL):  python3 intr-bash2.py [bash]
import os, pty, re, select, sys, time

CASES = [
    # (name, setup, text before ^C, text after ^C)
    ("c1", "", "if sleep 30; then echo y-c1; else echo n-c1; fi\n", 'echo "c1 st=$?"\n'),
    ("c2", "", "sleep 30 || echo or-c2\n", 'echo "c2 st=$?"\n'),
    ("c3", "f3() { sleep 30; echo in-c3; }\n", "f3\n", 'echo "c3 st=$?"\n'),
    ("c4", "", "( sleep 30; echo sub-c4 )\n", 'echo "c4 st=$?"\n'),
    ("c5", "", "sleep 30 | cat; echo after-c5\n", 'echo "c5 st=$?"\n'),
    ("c6", "f6() { sleep 30; }\n", "f6; echo after-c6\n", 'echo "c6 st=$?"\n'),
    ("c7", "f7() { if sleep 30; then echo y-c7; else echo n-c7; fi; }\n", "f7\n",
     'echo "c7 st=$?"\n'),
    ("c8", "", "{ sleep 30; echo grp-c8; }\n", 'echo "c8 st=$?"\n'),
    ("c9", "", "while sleep 30; do echo body-c9; done; echo after-c9\n",
     'echo "c9 st=$?"\n'),
    ("t1", "trap 'echo T-t1' INT\n",
     "for i in 1 2; do sleep 30; echo it-t1-$i; done; echo after-t1\n",
     'trap - INT; echo "t1 st=$?"\n'),
    ("t2", "trap 'echo T-t2' INT\n", "while :; do :; done; echo after-t2\n",
     'trap - INT; echo "t2 st=$?"\n'),
    ("t3", "trap 'echo T-t3' INT\n", "sleep 30; echo after-t3\n",
     'trap - INT; echo "t3 st=$?"\n'),
    ("s1", "printf 'sleep 30\\necho in-s1\\n' > /tmp/s1.sh\n", ". /tmp/s1.sh; echo after-s1\n",
     'echo "s1 st=$?"\n'),
    ("s2", "printf 'sleep 30\\necho in-s2\\n' > /tmp/s2.sh\n", ". /tmp/s2.sh\n",
     'echo "s2 st=$?"\n'),
]

bash = sys.argv[1] if len(sys.argv) > 1 else "bash"
pid, fd = pty.fork()
if pid == 0:
    os.environ["PS1"] = "$ "
    os.environ["PS2"] = "> "
    os.execvp(bash, [bash, "--norc", "--noprofile", "-i"])
out = b""
def pump(secs):
    global out
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: out += os.read(fd, 65536)
            except OSError: return
pump(1.0)
for name, setup, pre, post in CASES:
    if setup:
        os.write(fd, setup.encode()); pump(0.5)
    out = b""
    os.write(fd, pre.encode()); pump(1.0)
    os.write(fd, b"\x03"); pump(1.0)
    os.write(fd, post.encode()); pump(1.5)
    t = re.sub(r"\x1b\[\?2004[hl]", "", out.decode("utf-8", "replace")).replace("\r", "")
    lines = [l for l in t.split("\n")
             if l.strip() and not l.startswith("$ ") and not l.startswith("> ")
             and "echo " not in l]
    print("%-3s %s" % (name, " | ".join(lines)))
os.write(fd, b"exit\n"); pump(0.5)
