"""kshrun.py HOST PORT GUESTFILE [TIMEOUT] -- source GUESTFILE in the
in-kernel shell over TCP and print what comes back, up to a line
`KSH-END` (echoed after the source). The file must already be on the guest.
Prompts and the echoed command are left in: read the output for the
probe's own lines."""
import socket
import sys
import time

host, port, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
limit = float(sys.argv[4]) if len(sys.argv) > 4 else 60.0
s = socket.create_connection((host, port), timeout=10)
time.sleep(1.0)
s.sendall(b". " + path.encode() + b"; echo KSH-END\n")
s.settimeout(1.0)
buf = b""
end = time.time() + limit
while time.time() < end:
    try:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    except Exception:
        pass
    if b"\nKSH-END" in buf.replace(b"\r", b""):
        break
sys.stdout.write(buf.decode("utf-8", "replace").replace("\r", ""))
