"""Input-routing test: after chromewin has a CDP session (g_session set), inject
keystrokes to the focused Chromium window and verify chromewin forwards them to
chrome (the [chromewin] input probe). Keyboard needs no cursor positioning."""
import json, os, socket, subprocess, time
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
SERIAL = "logs/run47input.log"; PORT = 4464

def qmp(s, o):
    s.sendall((json.dumps(o) + "\r\n").encode()); b = b""
    while not b.endswith(b"\n"):
        c = s.recv(1)
        if not c: break
        b += c
    return b.decode(errors="replace").strip()

def main():
    os.chdir(r"C:\CustomOS\tobyOS")
    subprocess.run(["taskkill","/F","/IM","qemu-system-x86_64.exe"], capture_output=True); time.sleep(1)
    for f in ("logs/in_after.png",):
        if os.path.exists(f): os.remove(f)
    open(SERIAL,"w").close()
    cmd = [QEMU,"-cdrom","tobyOS.iso",
           "-drive","file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough","-boot","d",
           "-netdev","user,id=net0","-device","e1000,netdev=net0,mac=52:54:00:12:34:56",
           "-accel","whpx,kernel-irqchip=off","-smp","4","-m","4096","-cpu","qemu64,+smep,+smap",
           "-serial","file:"+SERIAL,"-qmp","tcp:127.0.0.1:%d,server,nowait"%PORT,"-no-reboot","-display","none"]
    print("launching qemu (WHPX)", flush=True)
    q = subprocess.Popen(cmd, stderr=subprocess.PIPE); s=None
    try:
        for _ in range(60):
            try: s = socket.create_connection(("127.0.0.1",PORT),timeout=2); break
            except OSError: time.sleep(0.5)
        b=b""
        while not b.endswith(b"\n"): b += s.recv(1)
        qmp(s, {"execute":"qmp_capabilities"})
        # wait for the CDP session (g_session set => key forwarding active)
        print("waiting for sessionId...", flush=True)
        for _ in range(120):
            try:
                if "[chromewin] sessionId" in open(SERIAL,errors="replace").read(): break
            except OSError: pass
            time.sleep(1)
        time.sleep(3)
        print("injecting keystrokes to focused window", flush=True)
        for k in ("h","e","l","l","o"):
            qmp(s, {"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":k}}}]}})
            qmp(s, {"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":k}}}]}})
            time.sleep(0.15)
        # best-effort mouse click on the page area (home then small steps)
        def rel(dx,dy): qmp(s,{"execute":"input-send-event","arguments":{"events":[{"type":"rel","data":{"axis":"x","value":dx}},{"type":"rel","data":{"axis":"y","value":dy}}]}})
        rel(-4000,-4000); time.sleep(0.3)
        for _ in range(120): rel(3,2)          # ~ into the page area
        time.sleep(0.3)
        qmp(s,{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":True}}]}}); time.sleep(0.1)
        qmp(s,{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":False}}]}})
        time.sleep(4)
        print("after:", qmp(s,{"execute":"screendump","arguments":{"filename":"logs/in_after.png","format":"png"}}), flush=True)
        qmp(s, {"execute":"quit"})
    finally:
        try: q.wait(timeout=10)
        except Exception: q.kill()
        subprocess.run(["taskkill","/F","/IM","qemu-system-x86_64.exe"], capture_output=True)

if __name__=="__main__": main()
