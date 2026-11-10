# intr-bash.py -- the ORACLE for the kernel shell's ^C (tools/agent/probes/
# b20/kintr.py): the same cases typed into an INTERACTIVE bash on a pty, ^C
# sent as the terminal sends it, only the marker lines kept. Run it where a
# bash and a pty are (WSL's Linux):
#
#   python3 intr-bash.py [bash]
import os, pty, re, select, sys, time

CASES = [
    # (name, text before ^C, seconds to wait before ^C, text after ^C)
    ("i1", "sleep 30\n", 1.0, 'echo "i1 st=$?"\n'),
    ("i2", "while :; do :; done\n", 1.0, 'echo "i2 st=$?"\n'),
    ("i3", "sleep 30; echo after-i3\n", 1.0, 'echo "i3 st=$?"\n'),
    ("i4", "rm -f /tmp/kif; mkfifo /tmp/kif; read x < /tmp/kif; echo after-i4\n", 1.0,
     'echo "i4 st=$?"\n'),
    ("i5", "echo partial-i5", 0.5, 'echo "i5 st=$?"\n'),
    ("i6", "if true\n", 0.5, 'echo "i6 st=$?"\n'),
    ("i7", "trap 'echo trapped-i7' INT; sleep 30; echo after-i7 st=$?\n", 1.0,
     "trap - INT\n"),
    ("i8", "sleep 30\n", 1.0, 'echo "ahead-i8 st=$?"\n'),
    ("i10", "for i in 1 2 3; do sleep 30; echo it$i; done; echo after-i10\n", 1.0,
     'echo "i10 st=$?"\n'),
    ("i11", "x=0; while [ $x -lt 3 ]; do sleep 30; x=$((x+1)); done; echo after-i11\n", 1.0,
     'echo "i11 st=$? x=$x"\n'),
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
            try:
                d = os.read(fd, 4096)
            except OSError:
                return
            if not d:
                return
            out += d
pump(1.0)
for name, before, wait, after in CASES:
    os.write(fd, before.encode())
    pump(wait)
    os.write(fd, b"\x03")
    pump(0.7)
    os.write(fd, after.encode())
    pump(1.0)
os.write(fd, b"rm -f /tmp/kif; exit\n")
pump(1.0)
text = out.decode("utf-8", "replace").replace("\r", "")
text = re.sub("\x1b\\[\\?2004[hl]", "", text)
marks = ("st=", "after-", "trapped-", "ahead-", "it1", "it2")
for line in text.split("\n"):
    l = line.strip()
    if l.startswith("$ ") or l.startswith("> "):
        continue                      # the typed command's echo
    if any(m in l for m in marks):
        print(l)
