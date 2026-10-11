# kintr2.py -- ^C at the KERNEL shell over TCP (the 0x03 a raw client sends),
# for the cases that tell bash's wait_for rule apart: a foreground command
# killed by ^C makes the shell act as if it had the SIGINT in a loop or a
# list (`;` `&&` `||`, a function's body, a group) or a sourced file, with
# INT untrapped; in a loop with it trapped; and not otherwise -- an `if`
# alone goes on to its else. The same cases and the same output form as
# tools/agent/probes/b21/intr-pty.py, so the lines diff against the guest's
# bash 5.2.15 there (logs/b22/intr-bash-*.out).
#
# Usage: python kintr2.py HOST [PORT]
import re, socket, sys, time
host = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 7777

CASES = [
    ("c1", "", "if sleep 30; then echo y-c1; else echo n-c1; fi\n", 1.0, 'echo "c1 st=$?"\n'),
    ("c2", "", "sleep 30 || echo or-c2\n", 1.0, 'echo "c2 st=$?"\n'),
    ("c3", "f3() { sleep 30; echo in-c3; }\n", "f3\n", 1.0, 'echo "c3 st=$?"\n'),
    ("c5", "", "sleep 30 | cat; echo after-c5\n", 1.0, 'echo "c5 st=$?"\n'),
    ("c6", "f6() { sleep 30; }\n", "f6; echo after-c6\n", 1.0, 'echo "c6 st=$?"\n'),
    ("c7", "f7() { if sleep 30; then echo y-c7; else echo n-c7; fi; }\n", "f7\n", 1.0,
     'echo "c7 st=$?"\n'),
    ("c8", "", "{ sleep 30; echo grp-c8; }\n", 1.0, 'echo "c8 st=$?"\n'),
    ("c9", "", "while sleep 30; do echo body-c9; done; echo after-c9\n", 1.0,
     'echo "c9 st=$?"\n'),
    ("c10", "", "for i in 1 2; do if sleep 30; then :; else echo else-c10-$i; fi; done; echo after-c10\n",
     1.0, 'echo "c10 st=$?"\n'),
    ("c11", "", "( sleep 30 ); echo after-c11\n", 1.0, 'echo "c11 st=$?"\n'),
    ("c12", "f12() { sleep $1; }\n", "for i in 30 2; do f12 $i; done\n", 1.0,
     'echo "c12 st=$?"\n'),
    ("c14", "", "sleep 30 && echo and-c14\n", 1.0, 'echo "c14 st=$?"\n'),
    ("c15", "", "sleep 30 | /bin/cat; echo after-c15\n", 1.0,
     'echo "c15 st=$? ps=${PIPESTATUS[*]}"\n'),
    ("c16", "", "sleep 30 | true; echo after-c16\n", 1.0, 'echo "c16 st=$?"\n'),
    ("c17", "true | false\n", "echo partial-c17", 0.5, 'echo "c17 ps=${PIPESTATUS[*]}"\n'),
    ("c18", "", "sleep 30 | cat\n", 1.0, 'echo "c18 ps=${PIPESTATUS[*]}"\n'),
    ("t5", "printf 'sleep 30\\necho in-t5\\n' > /tmp/t5.sh; trap 'echo T-t5' INT\n",
     ". /tmp/t5.sh; echo after-t5\n", 1.0, 'trap - INT; echo "t5 st=$?"\n'),
    ("t1", "trap 'echo T-t1' INT\n",
     "for i in 1 2; do sleep 30; echo it-t1-$i; done; echo after-t1\n", 1.0,
     'trap - INT; echo "t1 st=$?"\n'),
    ("t3", "trap 'echo T-t3' INT\n", "sleep 30; echo after-t3\n", 1.0,
     'trap - INT; echo "t3 st=$?"\n'),
    ("s1", "printf 'sleep 30\\necho in-s1\\n' > /tmp/s1.sh\n", ". /tmp/s1.sh; echo after-s1\n",
     1.0, 'echo "s1 st=$?"\n'),
    ("s2", "printf 'sleep 30\\necho in-s2\\n' > /tmp/s2.sh\n", ". /tmp/s2.sh\n", 1.0,
     'echo "s2 st=$?"\n'),
    ("i4", "", "rm -f /tmp/kif; mkfifo /tmp/kif; read x < /tmp/kif; echo after-i4\n", 1.5,
     'echo "i4 st=$?"\n'),
]

# What the guest's bash 5.2.15 prints on a pty for each case (logs/b22
# intr-bash-*.out): lines that must be there, and text never there -- the
# rest of a list a ^C ended, a trap that does not run, a builtin's error,
# and the kernel's own log lines, which are no shell's output.
EXPECT = {
    "c1": (["n-c1", "c1 st=0"], ["y-c1"]),
    "c2": (["c2 st=130"], ["or-c2"]),
    "c3": (["c3 st=130"], ["in-c3"]),
    "c5": (["c5 st=130"], ["after-c5", "read error"]),
    "c6": (["c6 st=130"], ["after-c6"]),
    "c7": (["n-c7", "c7 st=0"], ["y-c7"]),
    "c8": (["c8 st=130"], ["grp-c8"]),
    "c9": (["c9 st=130"], ["body-c9", "after-c9"]),
    "c10": (["c10 st=130"], ["else-c10", "after-c10"]),
    "c11": (["c11 st=130"], ["after-c11"]),
    "c12": (["c12 st=0"], []),
    "c14": (["c14 st=130"], ["and-c14"]),
    "c15": (["c15 st=130 ps=130"], ["after-c15"]),
    "c16": (["c16 st=130"], ["after-c16"]),
    "c17": (["c17 ps=130"], []),
    "c18": (["c18 ps=130 130"], []),
    "t5": (["in-t5", "after-t5", "t5 st=0"], ["T-t5"]),
    "t1": (["t1 st=0"], ["T-t1", "it-t1", "after-t1"]),
    "t3": (["after-t3", "t3 st=0"], ["T-t3"]),
    "s1": (["s1 st=130"], ["in-s1", "after-s1"]),
    "s2": (["s2 st=130"], ["in-s2"]),
    "i4": (["i4 st=130"], ["after-i4", "Interrupted", "I/O error"]),
}
NEVER_ANYWHERE = ["[elf]", "[proc]", "[cap]"]

# CASES=c17,c18 runs those alone
import os
if os.environ.get("CASES"):
    _only = set(os.environ["CASES"].split(","))
    CASES = [c for c in CASES if c[0] in _only]

s = socket.create_connection((host, port), timeout=10)
s.settimeout(0.2)
buf = b""
def take(secs):
    global buf
    end = time.time() + secs
    while time.time() < end:
        try:
            d = s.recv(65536)
            if not d: break
            buf += d
        except Exception:
            pass
    t = buf.decode("utf-8", "replace"); buf = b""
    return t
def send(t):
    s.sendall(t.encode())

PROMPT = re.compile(r"^(?:tobyOS> |> )+")
def lines_of(t, typed):
    t = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", t)
    res = []
    for l in t.split("\n"):
        l = l.rstrip("\r").split("\r")[-1]
        l = PROMPT.sub("", l).rstrip()
        if not l or l.strip() in typed: continue
        res.append(l)
    return res

take(1.5)
bad = []
for name, setup, pre, wait, post in CASES:
    typed = set(x.strip() for x in (setup + pre + post).split("\n") if x.strip())
    if setup:
        send(setup); take(0.8)
    send(pre); t = take(wait)
    send("\x03"); t += take(1.0)
    send(post); t += take(2.5)
    got = lines_of(t, typed)
    print("%-4s %s" % (name, " | ".join(got)), flush=True)
    want, never = EXPECT.get(name, ([], []))
    text = "\n".join(got)
    if any(w not in got for w in want) or \
       any(n in text for n in never + NEVER_ANYWHERE):
        bad.append(name)
# one long-lived shell: leave nothing behind
send("rm -f /tmp/s1.sh /tmp/s2.sh /tmp/t5.sh /tmp/kif; "
     "unset -f f3 f6 f7 f12; trap - INT\n"); take(1.0)
s.close()
print("KINTR2: " + ("ALL AS BASH" if not bad else "%d NOT AS BASH: %s"
                    % (len(bad), " ".join(bad))))
sys.exit(1 if bad else 0)
