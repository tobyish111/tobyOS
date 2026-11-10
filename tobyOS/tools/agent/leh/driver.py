#!/usr/bin/env python3
"""driver.py -- type the same keys at bash's readline and at tsh's line
editor (the leh harness), each on its own pty, and compare the line each
one accepted. bash's line is read back with `fc -ln -1`; every case's
line is a comment, so nothing it holds is run. History is the same three
entries on both sides."""
import os, pty, re, select, sys, time

def typekeys(fd, keys, gap=0.02):
    # one key at a time: both editors see the keys arrive as typed ones
    for i in range(len(keys)):
        os.write(fd, keys[i:i + 1])
        time.sleep(gap)

HIST = ["# first entry one two", "# echo alpha beta gamma", "# ls /usr/lib/x.tar.gz foo"]

def spawn(argv, env):
    pid, fd = pty.fork()
    if pid == 0:
        os.execvpe(argv[0], argv, env)
    return pid, fd

def readall(fd, until, limit=5.0):
    buf = b""
    end = time.time() + limit
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                break
            if not d:
                break
            buf += d
            if until(buf):
                # drain a moment more
                t2 = time.time() + 0.15
                while time.time() < t2:
                    r, _, _ = select.select([fd], [], [], 0.03)
                    if r:
                        try:
                            d = os.read(fd, 65536)
                        except OSError:
                            break
                        if not d:
                            break
                        buf += d
                return buf
    return buf

def bash_line(mode, keys, hist=HIST):
    env = dict(os.environ, PS1="$ ", INPUTRC="/dev/null", HISTFILE="/dev/null",
               TERM="xterm", LANG="C.UTF-8")
    pid, fd = spawn(["bash", "--norc", "--noprofile", "-i"], env)
    readall(fd, lambda b: b"$ " in b)
    os.write(fd, ("set -o %s; bind 'set enable-bracketed-paste off'\r" % mode).encode())
    readall(fd, lambda b: b.count(b"$ ") >= 1, 1.0)
    os.write(fd, b"history -c\r")
    readall(fd, lambda b: b"$ " in b, 1.0)
    for h in hist:
        os.write(fd, ("history -s %s\r" % shq(h)).encode())
        readall(fd, lambda b: b"$ " in b, 1.0)
    typekeys(fd, keys)
    readall(fd, lambda b: b"$ " in b, 2.0)
    os.write(fd, b"printf '<<L:%s>>\\n' \"$(fc -ln -1)\"\r")
    out = readall(fd, lambda b: re.search(rb"<<L:(?!%s)", b) is not None, 3.0)
    os.write(fd, b"exit\r")
    time.sleep(0.1)
    try:
        os.kill(pid, 9)
    except OSError:
        pass
    os.waitpid(pid, 0)
    m = [x for x in re.findall(rb"<<L:(.*?)>>", out, re.S) if x != b"%s"]
    if not m:
        return None
    return m[-1].decode("utf-8", "replace").lstrip("\t ").replace("\r", "")

def tsh_line(mode, keys, hist=HIST):
    env = dict(os.environ, TERM="xterm", COLUMNS="80", LEH_HIST="\n".join(hist))
    pid, fd = spawn(["./leh"] + (["vi"] if mode == "vi" else []), env)
    readall(fd, lambda b: b"$ " in b)
    typekeys(fd, keys)
    out = readall(fd, lambda b: b"<<LINE:" in b and b">>" in b.split(b"<<LINE:")[-1], 3.0)
    try:
        os.kill(pid, 9)
    except OSError:
        pass
    os.waitpid(pid, 0)
    m = re.findall(rb"<<LINE:(.*?)>>", out, re.S)
    return m[-1].decode("utf-8", "replace").lstrip("	 ") if m else None

def shq(t):
    return "'" + t.replace("'", "'\\''") + "'"

H2 = ["ls -l /tmp", "echo 'a b' \"c d\" $(e f) x>y", "cat foo|grep bar # note"]

E = b"\x1b"
CASES = [
 # (mode, keys, name)
 ("vi", b"# abc def ghi" + E + b"bcwXYZ" + E + b"\r", "cw on a word"),
 ("vi", b"# abc def ghi" + E + b"0wwdw\r", "dw"),
 ("vi", b"# abc def ghi" + E + b"02wD\r", "D"),
 ("vi", b"# abc def ghi" + E + b"0fdx\r", "f then x"),
 ("vi", b"# abc def ghi" + E + b"0tex\r", "t then x"),
 ("vi", b"# abc def ghi" + E + b"Fax;x\r", "F a ; repeat"),
 ("vi", b"# abc def ghi" + E + b"03xu\r", "x count then undo"),
 ("vi", b"# abc def" + E + b"0ll~~~\r", "tilde"),
 ("vi", b"# abc def" + E + b"bdwP\r", "dw then P"),
 ("vi", b"# abc def" + E + b"0wywA " + E + b"p\r", "yw then p"),
 ("vi", b"# one two three" + E + b"0wcwX" + E + b"w.\r", "dot repeat cw"),
 ("vi", b"# one two three" + E + b"0wx..\r", "dot repeat x"),
 ("vi", b"# one two three" + E + b"0w2dw\r", "2dw"),
 ("vi", b"# one two three" + E + b"0wd2w\r", "d2w"),
 ("vi", b"# one two three" + E + b"0wrZ\r", "r"),
 ("vi", b"# one two three" + E + b"0w3rZ\r", "3r"),
 ("vi", b"# one two three" + E + b"0wsQ" + E + b"\r", "s"),
 ("vi", b"# one two three" + E + b"0wSnew" + E + b"\r", "S"),
 ("vi", b"# one two three" + E + b"0wCend" + E + b"\r", "C"),
 ("vi", b"# one two three" + E + b"0wiIN" + E + b"aAP" + E + b"\r", "i then a"),
 ("vi", b"# one two three" + E + b"IA" + E + b"Az" + E + b"\r", "I and A"),
 ("vi", b"# one.two-three" + E + b"0wwwD\r", "w over punctuation"),
 ("vi", b"# one.two-three" + E + b"0WWD\r", "W"),
 ("vi", b"# one.two three" + E + b"bbD\r", "b over punctuation"),
 ("vi", b"# one.two three" + E + b"0eeD\r", "e"),
 ("vi", b"# one.two three" + E + b"0EaX" + E + b"\r", "E then a"),
 ("vi", b"# (a [b] c) d" + E + b"0f(%x\r", "percent"),
 ("vi", b"# abc def" + E + b"0wd$\r", "d$"),
 ("vi", b"# abc def" + E + b"$d0\r", "d0"),
 ("vi", b"# abc def" + E + b"0wd^\r", "d^"),
 ("vi", b"# abc def ghi" + E + b"0wcbX" + E + b"\r", "cb"),
 ("vi", b"# abc def ghi" + E + b"0wdeP\r", "de then P"),
 ("vi", b"# abc def ghi" + E + b"0wwdbx\r", "db"),
 ("vi", b"# abc def ghi" + E + b"0dd" + b"i# new" + E + b"\r", "dd"),
 ("vi", b"# abc def ghi" + E + b"0wwccrepl" + E + b"\r", "cc"),
 ("vi", b"# abc" + E + b"Y$p\r", "Y p"),
 ("vi", b"# abc def" + E + b"0wyeP\r", "ye P"),
 ("vi", b"# abc def" + E + b"0w3|x\r", "column"),
 ("vi", b"# abc def" + E + b"0wmaw`ax\r", "marks"),
 ("vi", b"# abc def" + E + b"0wXX\r", "X"),
 ("vi", b"# abcdef" + E + b"0lllRxyz" + E + b"\r", "R"),
 ("vi", E + b"k\r", "k history"),
 ("vi", E + b"kk\r", "kk history"),
 ("vi", E + b"kkj\r", "kkj history"),
 ("vi", E + b"2G\r", "2G"),
 ("vi", E + b"G\r", "G"),
 ("vi", E + b"/alpha\rx\r", "slash search then x"),
 ("vi", E + b"/one\rn\r", "search n"),
 ("vi", b"# x" + E + b"0wA" + E + b"_\r", "underscore yank arg"),
 ("vi", b"echo hi" + E + b"#", "hash comment"),
 ("vi", b"# abc def" + E + b"0wdwU\r", "U revert"),
 ("vi", b"# one" + E + b"a two\x17three\r", "insert ^W"),
 ("vi", b"# one two\x15# again\r", "insert ^U"),
 ("vi", b"# ab\x16\x01c\r", "insert ^V"),
 ("vi", b"# a\x01b\r", "insert ^A literal"),
 ("emacs", b"# abc def ghi\x1bb\x1bb\x1bd\r", "M-b M-d"),
 ("emacs", b"# abc def\x01\x1bf\x1bf\x0b\r", "M-f ^K"),
 ("emacs", b"# abc def\x17\x17\x19\r", "^W ^W ^Y"),
 ("emacs", b"# abc def\x1bu\r", "M-u at end"),
 ("emacs", b"# abc def\x01\x1bf\x1bf\x1bb\x1bu\r", "M-u"),
 ("emacs", b"# abc def\x01\x1bf\x1bc\x1bl\r", "M-c M-l"),
 ("emacs", b"# abc def\x1bt\r", "M-t"),
 ("emacs", b"# abc def\x1f\r", "undo"),
 ("emacs", b"# abc\x08\x08\x1f\r", "undo backspace"),
 ("emacs", b"# \x1b.\r", "M-."),
 ("emacs", b"# \x1b.\x1b.\r", "M-. M-."),
 ("emacs", b"# abc def\x01\x1d d\x04\r", "^] search ^D"),
 ("emacs", b"# one\x17two\x17\x19\x1by\r", "yank-pop"),
 ("emacs", b"# abc   def\x1bb\x08\x08\x1b\\\r", "M-backslash"),
 ("emacs", b"\x10\x10\r", "^P ^P"),
 ("emacs", b"\x1b<\r", "M-<"),
 ("emacs", b"# abc\x1br\r", "M-r revert"),
 ("emacs", b"# abc\x16\x01d\r", "^V"),
 ("emacs", b"# \x1b.\r", "M-. H2", H2),
 ("emacs", b"# \x1b.\x1b.\r", "M-. M-. H2", H2),
 ("emacs", b"# \x1b.\x1b.\x1b.\r", "M-. x3 H2", H2),
 ("emacs", b"# \x1b.\x1b.\x1b.\x1b.\r", "M-. x4 H2", H2),
 ("emacs", b"# \x1b\x19\r", "M-^Y H2", H2),
 ("emacs", b"# z\x1b.\x02\x1b.\r", "M-. move M-. H2", H2),
 ("vi", b"# x" + E + b"_\r", "underscore H2", H2),
 ("vi", b"# x" + E + b"2_\r", "2 underscore H2", H2),
 ("vi", b"# x" + E + b"9_\r", "9 underscore H2", H2),
 ("emacs", b"# one two\x17\x17\x19\r", "^W ^W ^Y again"),
 ("emacs", b"# one two\x01\x1bf\x0b\x01\x1bd\x05\x19\r", "kill then move kill"),
 ("emacs", b"# one two three\x17\x1b\x7f\x19\r", "^W M-DEL join"),
 ("vi", b"# one two three" + E + b"0wdwdwP\r", "vi dw dw P"),
 ("vi", b"# one two" + E + b"0wiX" + E + b"xU\r", "U after insert"),
 ("vi", b"# one two" + E + b"0wxiY" + E + b"xU\r", "U second cmd entry"),
]

def main():
    sel = sys.argv[1:]
    bad = 0
    for i, case in enumerate(CASES):
        mode, keys, name = case[:3]
        hist = case[3] if len(case) > 3 else HIST
        if sel and str(i) not in sel and name not in sel:
            continue
        b = bash_line(mode, keys, hist)
        t = tsh_line(mode, keys, hist)
        ok = b == t
        # bash puts no empty line in its history: `fc -ln -1` then shows
        # the entry before, unchanged -- the same as tsh accepting ''
        if not ok and t == "" and b == hist[-1]:
            ok = True
        if not ok:
            bad += 1
        print("%-3d %-4s %-26s %s  bash=%r tsh=%r" % (i, mode, name, "OK  " if ok else "DIFF", b, t))
    print("DIFFERENT: %d" % bad)

main()
