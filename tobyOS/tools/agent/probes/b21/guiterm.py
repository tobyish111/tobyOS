# guiterm.py -- drive the DESKTOP's terminal window (/bin/gui_term) in QEMU:
# the keyboard through QMP, the screen through QMP screendump. What an SSH
# pty cannot show: the window runs /bin/tsh on a pty of its own, launched by
# pid 0 (the desktop's launcher), so the session and controlling terminal
# its programs inherit are not an SSH session's.
#
# Usage (one QEMU, many steps; QMP on 127.0.0.1:4490, SSH 2222, kshell 17777):
#   python guiterm.py boot ISO LOG      boot ISO, wait for SSH, leave it running
#   python guiterm.py shot FILE.png     screendump
#   python guiterm.py type TEXT         type TEXT ("\n" = Enter)
#   python guiterm.py key NAME...       press keys: spc ret down up ctrl-c q ...
#   python guiterm.py kshell CMD        send CMD (a line) to the kernel shell
#   python guiterm.py quit              stop QEMU
import json, os, socket, subprocess, sys, time
QMP_PORT = 4490
ROOT = r"C:\CustomOS\tobyOS"
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"

def qmp_open():
    s = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
    f = s.makefile("rb")
    f.readline()                                    # the greeting
    qmp(s, f, {"execute": "qmp_capabilities"})
    return s, f

def qmp(s, f, o):
    s.sendall((json.dumps(o) + "\r\n").encode())
    while True:
        l = f.readline()
        if not l: return None
        r = json.loads(l)
        if "return" in r or "error" in r: return r   # skip events

SHIFTED = {'|': 'backslash', '$': '4', '?': 'slash', '"': 'apostrophe',
           '>': 'dot', '<': 'comma', '_': 'minus', '&': '7', '!': '1',
           '*': '8', '(': '9', ')': '0', ':': 'semicolon', '+': 'equal',
           '{': 'bracket_left', '}': 'bracket_right', '~': 'grave_accent',
           '@': '2', '#': '3', '%': '5', '^': '6'}
PLAIN = {' ': 'spc', '\n': 'ret', '=': 'equal', '-': 'minus', '/': 'slash',
         '.': 'dot', ',': 'comma', ';': 'semicolon', "'": 'apostrophe',
         '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash',
         '`': 'grave_accent', '\t': 'tab'}

def ev(code, down):
    return {"type": "key", "data": {"down": down,
            "key": {"type": "qcode", "data": code}}}

def press(s, f, code, mods=()):
    for m in mods: qmp(s, f, {"execute": "input-send-event", "arguments": {"events": [ev(m, True)]}})
    qmp(s, f, {"execute": "input-send-event", "arguments": {"events": [ev(code, True)]}})
    time.sleep(0.03)
    qmp(s, f, {"execute": "input-send-event", "arguments": {"events": [ev(code, False)]}})
    for m in reversed(mods): qmp(s, f, {"execute": "input-send-event", "arguments": {"events": [ev(m, False)]}})
    time.sleep(0.06)

def type_text(s, f, t):
    for c in t:
        if 'a' <= c <= 'z' or '0' <= c <= '9': press(s, f, c)
        elif 'A' <= c <= 'Z': press(s, f, c.lower(), ("shift",))
        elif c in PLAIN: press(s, f, PLAIN[c])
        elif c in SHIFTED: press(s, f, SHIFTED[c], ("shift",))
        else: raise SystemExit("no key for %r" % c)

def main():
    os.chdir(ROOT)
    cmd = sys.argv[1]
    if cmd == "boot":
        iso, log = sys.argv[2], sys.argv[3]
        subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"], capture_output=True)
        time.sleep(1)
        if os.path.exists(log): os.remove(log)
        subprocess.Popen([QEMU, "-cdrom", iso,
            "-drive", "file=disk.img,format=raw,if=ide,index=0,media=disk", "-boot", "d",
            "-smp", "4", "-m", "5120",
            "-netdev", "user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::17777-:7777",
            "-device", "e1000,netdev=net0,mac=52:54:00:12:34:56",
            "-serial", "file:" + log, "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
            "-no-reboot", "-display", "none"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=0x00000008)               # DETACHED_PROCESS
        for i in range(100):
            try:
                c = socket.create_connection(("127.0.0.1", 2222), timeout=3)
                c.settimeout(5); b = c.recv(64); c.close()
                if b.startswith(b"SSH-"): print("qemu up"); return
            except Exception: pass
            time.sleep(3)
        print("qemu NOT up"); sys.exit(1)
    if cmd == "kshell":
        c = socket.create_connection(("127.0.0.1", 17777), timeout=10)
        time.sleep(1.0)
        c.sendall((sys.argv[2] + "\n").encode())
        c.settimeout(0.5); out = b""; end = time.time() + 3
        while time.time() < end:
            try:
                d = c.recv(4096)
                if not d: break
                out += d
            except Exception: pass
        c.close()
        print(out.decode("utf-8", "replace").replace("\r", ""))
        return
    s, f = qmp_open()
    if cmd == "shot":
        p = os.path.abspath(sys.argv[2]).replace("\\", "/")
        print(qmp(s, f, {"execute": "screendump", "arguments": {"filename": p, "format": "png"}}))
    elif cmd == "type":
        type_text(s, f, sys.argv[2].replace("\\n", "\n"))
    elif cmd == "key":
        for k in sys.argv[2:]:
            if k.startswith("ctrl-"): press(s, f, k[5:], ("ctrl",))
            else: press(s, f, k)
    elif cmd == "quit":
        qmp(s, f, {"execute": "quit"})
    s.close()

main()
