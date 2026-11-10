# intr-pty.py -- ^C at an INTERACTIVE shell on a pty, case by case: the
# hosted tsh (an SSH session with -tt, which is what the desktop's terminal
# window runs too) or, as the oracle, the guest's own bash 5.2.15 started
# in the same session. Batch 20's cases (b20/kintr.py), and the ones that
# tell bash's wait_for rule apart: a foreground job-control job killed by
# SIGINT makes the shell act as if it had the SIGINT in a loop or a list
# (or a sourced file), with INT untrapped; in a loop with it trapped; and
# not otherwise -- an `if` alone goes on to its else.
#
# Usage: python intr-pty.py HOST PORT tsh|bash  > out
#   each case prints its lines (prompts and the typed text taken out), so
#   the two shells' outputs diff line for line.
import os, re, subprocess, sys, time, threading
host, port, shell = sys.argv[1], sys.argv[2], sys.argv[3]

CASES = [
    # (name, setup, text before ^C, seconds before ^C, text after ^C)
    ("i1", "", "sleep 30\n", 1.0, 'echo "i1 st=$?"\n'),
    ("i2", "", "while :; do :; done\n", 1.0, 'echo "i2 st=$?"\n'),
    ("i3", "", "sleep 30; echo after-i3\n", 1.0, 'echo "i3 st=$?"\n'),
    ("i4", "", "rm -f /tmp/kif; mkfifo /tmp/kif; read x < /tmp/kif; echo after-i4\n", 1.5,
     'echo "i4 st=$?"\n'),
    ("i5", "", "echo partial-i5", 0.5, 'echo "i5 st=$?"\n'),
    ("i6", "", "if true\n", 0.5, 'echo "i6 st=$?"\n'),
    ("i7", "", "trap 'echo trapped-i7' INT; sleep 30; echo after-i7 st=$?\n", 1.0,
     'trap - INT\n'),
    ("i10", "", "for i in 1 2 3; do sleep 30; echo it$i; done; echo after-i10\n", 1.0,
     'echo "i10 st=$?"\n'),
    ("i11", "", "x=0; while [ $x -lt 3 ]; do sleep 30; x=$((x+1)); done; echo after-i11\n",
     1.0, 'echo "i11 st=$? x=$x"\n'),
    ("c1", "", "if sleep 30; then echo y-c1; else echo n-c1; fi\n", 1.0, 'echo "c1 st=$?"\n'),
    ("c2", "", "sleep 30 || echo or-c2\n", 1.0, 'echo "c2 st=$?"\n'),
    ("c3", "f3() { sleep 30; echo in-c3; }\n", "f3\n", 1.0, 'echo "c3 st=$?"\n'),
    ("c4", "", "( sleep 30; echo sub-c4 )\n", 1.0, 'echo "c4 st=$?"\n'),
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
    # a function resets the loop level: no list, no loop inside f12 -- the
    # loop goes on to `sleep 2` (bash's loop_level = 0 in a function)
    ("c12", "f12() { sleep $1; }\n", "for i in 30 2; do f12 $i; done\n", 1.0,
     'echo "c12 st=$?"\n'),
    ("c13", "", "{ sleep 30; } | cat; echo after-c13\n", 1.0, 'echo "c13 st=$?"\n'),
    ("c14", "", "sleep 30 && echo and-c14\n", 1.0, 'echo "c14 st=$?"\n'),
    ("c15", "", "sleep 30 | /bin/cat; echo after-c15\n", 1.0,
     'echo "c15 st=$? ps=${PIPESTATUS[*]}"\n'),
    ("c16", "", "sleep 30 | true; echo after-c16\n", 1.0, 'echo "c16 st=$?"\n'),
    # PIPESTATUS after the shell is interrupted: bash's throw_to_top_level
    # makes it the one 130; a pipeline alone keeps its own
    ("c17", "true | false\n", "echo partial-c17", 0.5, 'echo "c17 ps=${PIPESTATUS[*]}"\n'),
    ("c18", "", "sleep 30 | cat\n", 1.0, 'echo "c18 ps=${PIPESTATUS[*]}"\n'),
    ("t5", "printf 'sleep 30\\necho in-t5\\n' > /tmp/t5.sh; trap 'echo T-t5' INT\n",
     ". /tmp/t5.sh; echo after-t5\n", 1.0,
     'trap - INT; echo "t5 st=$?"\n'),
    ("t1", "trap 'echo T-t1' INT\n",
     "for i in 1 2; do sleep 30; echo it-t1-$i; done; echo after-t1\n", 1.0,
     'trap - INT; echo "t1 st=$?"\n'),
    ("t3", "trap 'echo T-t3' INT\n", "sleep 30; echo after-t3\n", 1.0,
     'trap - INT; echo "t3 st=$?"\n'),
    ("s1", "printf 'sleep 30\\necho in-s1\\n' > /tmp/s1.sh\n", ". /tmp/s1.sh; echo after-s1\n",
     1.0, 'echo "s1 st=$?"\n'),
    ("s2", "printf 'sleep 30\\necho in-s2\\n' > /tmp/s2.sh\n", ". /tmp/s2.sh\n", 1.0,
     'echo "s2 st=$?"\n'),
    # last: a trapped INT in a loop of builtins runs the trap and goes on
    ("t2", "trap 'echo T-t2' INT\n", "while :; do :; done; echo after-t2\n", 1.0, ""),
]

# `intr-pty.py HOST PORT SHELL z`: the ^Z cases instead -- $? after a stop
if len(sys.argv) > 4 and sys.argv[4] == "z":
    CASES = [
        ("z1", "", "sleep 30\n", 1.0, 'echo "z1 st=$? ps=${PIPESTATUS[*]}"; kill -9 %1; wait\n'),
        ("z2", "", "seq 1 200 | less\n", 2.0, 'echo "z2 st=$? ps=${PIPESTATUS[*]}"; kill -9 %1; wait\n'),
        ("z3", "", "sleep 30 | cat\n", 1.0, 'echo "z3 st=$? ps=${PIPESTATUS[*]}"; kill -9 %1; wait\n'),
        # the first element ended 3: bash's 128 + WSTOPSIG of its exit status
        ("z4", "", "sh -c 'exit 3' | sleep 30\n", 1.0, 'echo "z4 st=$? ps=${PIPESTATUS[*]}"; kill -9 %1; wait\n'),
        # stopped again under fg
        ("z5", "", "seq 1 200 | less\n", 2.0, 'fg\n'),
        ("z6", "", "", 0.1, 'echo "z6 st=$? ps=${PIPESTATUS[*]}"; kill -9 %1; wait\n'),
    ]

# CASES=c17,c18 runs those alone
if os.environ.get("CASES"):
    _only = set(os.environ["CASES"].split(","))
    CASES = [c for c in CASES if c[0] in _only]

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
def take(secs):
    time.sleep(secs)
    with lock:
        b = b"".join(buf); buf.clear()
    return b.decode("utf-8", "replace")
def send(s):
    p.stdin.write(s.encode()); p.stdin.flush()

PROMPT = re.compile(r"^(?:(?:bash|tsh)-5\.2[#$] |> )+")
def lines_of(t, typed):
    t = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", t)
    res = []
    for l in t.split("\n"):
        # a line editor redraws with a bare CR: what shows is the last
        l = l.rstrip("\r").split("\r")[-1]
        l = PROMPT.sub("", l).rstrip()
        if not l or l.strip() in typed: continue
        res.append(l)
    return res

take(4.0)
if shell == "bash":
    send("exec /bin/bash --norc --noprofile -i\n"); take(3.0)
send("PS1='tsh-5.2# '; PS2='> '\n" if shell == "tsh" else "PS1='bash-5.2# '; PS2='> '\n")
take(1.0)
for name, setup, pre, wait, post in CASES:
    typed = set(s.strip() for s in (setup + pre + post).split("\n") if s.strip())
    if setup:
        send(setup); take(0.8)
    send(pre); t = take(wait)
    send("\x1a" if name.startswith("z") else "\x03"); t += take(1.0)
    if post:
        send(post); t += take(2.0)
    print("%-4s %s" % (name, " | ".join(lines_of(t, typed))), flush=True)
try: p.kill()
except Exception: pass
