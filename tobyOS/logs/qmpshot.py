#!/usr/bin/env python3
"""Take a QMP screendump. MSYS bash's /dev/tcp is unreliable here -- it
returns "Invalid argument" reading the greeting -- so the socket work is
done from Python instead."""
import socket, json, sys

host, port, out = "127.0.0.1", int(sys.argv[1]), sys.argv[2]
s = socket.create_connection((host, port), timeout=15)
f = s.makefile("rw", encoding="utf-8", newline="\n")
f.readline()                                   # greeting
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush()
f.readline()
f.write(json.dumps({"execute": "screendump",
                    "arguments": {"filename": out, "format": "png"}}) + "\n")
f.flush()
print(f.readline().strip())
s.close()
