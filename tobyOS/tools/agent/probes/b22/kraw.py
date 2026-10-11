# kraw.py -- the kernel shell over TCP, by hand: send each argument (a "^C"
# argument sends the 0x03 byte, "+N" waits N seconds), printing every byte
# that comes back with its time. For a ^C case kintr2.py shows wrong.
#
# Usage: python kraw.py HOST PORT ARG...
import socket, sys, time
host, port = sys.argv[1], int(sys.argv[2])
s = socket.create_connection((host, port), timeout=10)
s.settimeout(0.1)
t0 = time.time()
def pump(secs):
    end = time.time() + secs
    while time.time() < end:
        try:
            d = s.recv(65536)
            if not d: return
            print("%6.2f <- %r" % (time.time() - t0, d), flush=True)
        except Exception:
            pass
pump(1.5)
for a in sys.argv[3:]:
    if a.startswith("+"):
        pump(float(a[1:])); continue
    data = b"\x03" if a == "^C" else (a + "\n").encode()
    print("%6.2f -> %r" % (time.time() - t0, data), flush=True)
    s.sendall(data)
    pump(0.2)
pump(3.0)
s.close()
