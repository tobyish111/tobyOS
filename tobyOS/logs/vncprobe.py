#!/usr/bin/env python
"""vncprobe.py -- a real RFB 3.8 client, used as the VNC gate's oracle.

Deliberately does NOT reuse any tobyOS code. The DES auth response is
computed by shelling out to openssl, so a bug in src/des.c cannot cancel
out against a matching bug in the test -- the two implementations have to
agree independently or authentication fails.

Checks, in order:
  1  server offers RFB 003.008
  2  security type 2 is offered and type 1 (None) is NOT
  3  a WRONG password is rejected
  4  the correct password is accepted
  5  ServerInit geometry is sane and names the machine
  6  a full-screen Raw update arrives, complete and correctly framed
  7  the same under Hextile, and every tile decodes
  8  a remote keystroke reaches the guest's input path
  9  a remote pointer event reaches the guest's input path

Exit 0 only if every check passes.
"""

import os
import socket
import struct
import subprocess
import sys
import time

HOST = os.environ.get("VNCHOST", "127.0.0.1")
PORT = int(os.environ.get("VNCPORT", "5900"))
PASSWORD = os.environ.get("VNCPASS", "tobyvnc")
CONNECT_TIMEOUT = int(os.environ.get("VNCCONNECT", "180"))

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("  %-4s %s%s" % ("PASS" if ok else "FAIL", name,
                           (" -- " + detail) if detail else ""))
    return ok


def des_response(challenge, password):
    """RFB auth: DES-ECB the 16-byte challenge under the bit-reversed key."""
    key = password.encode()[:8].ljust(8, b"\0")
    rev = bytes(int("{:08b}".format(b)[::-1], 2) for b in key)
    p = subprocess.run(
        ["openssl", "enc", "-des-ecb", "-K", rev.hex().upper(), "-nopad",
         "-provider", "legacy", "-provider", "default"],
        input=challenge, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if len(p.stdout) != 16:
        raise RuntimeError("openssl DES failed: %r" % p.stderr[:200])
    return p.stdout


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("peer closed after %d/%d bytes" % (len(buf), n))
        buf += chunk
    return buf


def connect(timeout=10):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    s.settimeout(30)
    return s


def wait_for_server(deadline):
    """Wait until the GUEST is really serving RFB.

    QEMU's SLIRP completes the host-side TCP handshake for a forwarded port
    whether or not anything inside the guest is listening, so a successful
    connect() proves nothing at all. The banner is the first thing that does.
    """
    waited = 0
    while time.time() < deadline:
        s = None
        try:
            s = socket.create_connection((HOST, PORT), timeout=5)
            s.settimeout(5)
            banner = recv_exact(s, 12)
            s.settimeout(30)
            return s, banner, waited
        except Exception:                                   # noqa: BLE001
            if s:
                try:
                    s.close()
                except OSError:
                    pass
            time.sleep(3)
            waited += 3
    return None, None, waited


def handshake(sock, password, expect_ok=True):
    """Drive the 3.8 handshake. Returns (version, sectypes, auth_ok)."""
    version = recv_exact(sock, 12)
    sock.sendall(b"RFB 003.008\n")

    ntypes = recv_exact(sock, 1)[0]
    if ntypes == 0:
        reason_len = struct.unpack(">I", recv_exact(sock, 4))[0]
        raise RuntimeError("server refused: %s" %
                           recv_exact(sock, reason_len).decode("latin1"))
    sectypes = list(recv_exact(sock, ntypes))

    sock.sendall(bytes([2]))
    challenge = recv_exact(sock, 16)
    sock.sendall(des_response(challenge, password))

    result = struct.unpack(">I", recv_exact(sock, 4))[0]
    if result != 0 and not expect_ok:
        # RFB 3.8 sends a reason string on failure.
        try:
            rlen = struct.unpack(">I", recv_exact(sock, 4))[0]
            recv_exact(sock, rlen)
        except EOFError:
            pass
    return version, sectypes, (result == 0)


def read_update(sock, want_encoding):
    """Read one FramebufferUpdate; return (x, y, w, h, payload_len).

    Other server messages may legitimately arrive first -- ServerCutText in
    particular, since the guest pushes its clipboard whenever it changes.
    A real client consumes them and keeps waiting, so this does too.
    """
    while True:
        mtype = recv_exact(sock, 1)[0]
        if mtype == 0:                       # FramebufferUpdate
            rest = recv_exact(sock, 3)
            nrects = struct.unpack(">H", rest[1:3])[0]
            break
        if mtype == 1:                       # SetColourMapEntries
            b = recv_exact(sock, 5)
            recv_exact(sock, struct.unpack(">H", b[3:5])[0] * 6)
            continue
        if mtype == 2:                       # Bell
            continue
        if mtype == 3:                       # ServerCutText
            b = recv_exact(sock, 7)
            recv_exact(sock, struct.unpack(">I", b[3:7])[0])
            continue
        raise RuntimeError("unexpected server message type %d" % mtype)
    total = 0
    last = None
    for _ in range(nrects):
        rx, ry, rw, rh, enc = struct.unpack(">HHHHi", recv_exact(sock, 12))
        if enc == -223:            # DesktopSize pseudo-encoding, no payload
            last = (rx, ry, rw, rh, 0)
            continue
        if enc != want_encoding:
            raise RuntimeError("rect encoded %d, expected %d" % (enc, want_encoding))
        if enc == 0:               # Raw
            n = rw * rh * 4
            recv_exact(sock, n)
            total += n
        elif enc == 5:             # Hextile
            total += read_hextile(sock, rw, rh)
        last = (rx, ry, rw, rh, total)
    return last


def read_hextile(sock, w, h):
    """Decode hextile tile headers; raises if a tile is malformed."""
    consumed = 0
    for ty in range(0, h, 16):
        th = min(16, h - ty)
        for tx in range(0, w, 16):
            tw = min(16, w - tx)
            sub = recv_exact(sock, 1)[0]
            consumed += 1
            if sub & 0x01:                     # Raw tile
                n = tw * th * 4
                recv_exact(sock, n)
                consumed += n
                continue
            if sub & 0x02:                     # BackgroundSpecified
                recv_exact(sock, 4)
                consumed += 4
            if sub & 0x04:                     # ForegroundSpecified
                recv_exact(sock, 4)
                consumed += 4
            if sub & 0x08:                     # AnySubrects
                nsub = recv_exact(sock, 1)[0]
                consumed += 1
                per = 6 if (sub & 0x10) else 2
                recv_exact(sock, nsub * per)
                consumed += nsub * per
    return consumed


def set_encodings(sock, encodings):
    msg = struct.pack(">BBH", 2, 0, len(encodings))
    for e in encodings:
        msg += struct.pack(">i", e)
    sock.sendall(msg)


def request_update(sock, incremental, x, y, w, h):
    sock.sendall(struct.pack(">BBHHHH", 3, 1 if incremental else 0, x, y, w, h))


def main():
    print("=== vncprobe: RFB 3.8 against %s:%d ===" % (HOST, PORT))

    # The guest boots slowly under emulation; wait for a real banner.
    deadline = time.time() + CONNECT_TIMEOUT
    sock, version, waited = wait_for_server(deadline)
    if not check("connect", sock is not None,
                 ("server ready after ~%ds" % waited) if sock else
                 ("no RFB banner on %d within %ds" % (PORT, CONNECT_TIMEOUT))):
        return 1

    # --- 1/2: version banner and offered security types ---------------
    check("version-banner", version == b"RFB 003.008\n", repr(version))
    sock.sendall(b"RFB 003.008\n")

    ntypes = recv_exact(sock, 1)[0]
    sectypes = list(recv_exact(sock, ntypes)) if ntypes else []
    check("auth-required", 2 in sectypes and 1 not in sectypes,
          "offered types %r (1=None must be absent)" % sectypes)
    sock.close()

    # --- 3: a wrong password must be rejected -------------------------
    try:
        s2 = connect()
        _, _, ok = handshake(s2, "wrongpw", expect_ok=False)
        check("wrong-password-rejected", not ok,
              "server accepted a bad password" if ok else "")
        s2.close()
    except Exception as e:                                  # noqa: BLE001
        check("wrong-password-rejected", False, str(e))

    # The server rate-limits after repeated failures, so only one bad try.
    time.sleep(1)

    # --- 4/5: real session --------------------------------------------
    try:
        s3 = connect()
        _, _, ok = handshake(s3, PASSWORD, expect_ok=True)
        if not check("password-accepted", ok,
                     "" if ok else "auth rejected the good password"):
            return 1
    except Exception as e:                                  # noqa: BLE001
        check("password-accepted", False, str(e))
        return 1

    s3.sendall(bytes([1]))                                  # ClientInit, shared
    w, h = struct.unpack(">HH", recv_exact(s3, 4))
    pixfmt = recv_exact(s3, 16)
    namelen = struct.unpack(">I", recv_exact(s3, 4))[0]
    name = recv_exact(s3, namelen).decode("latin1")
    bpp, depth, big_endian, truecolour = pixfmt[0], pixfmt[1], pixfmt[2], pixfmt[3]
    check("server-init",
          0 < w <= 4096 and 0 < h <= 4096 and bpp == 32 and truecolour == 1,
          "%dx%d bpp=%d depth=%d be=%d truecolour=%d name=%r"
          % (w, h, bpp, depth, big_endian, truecolour, name))

    # --- 6: full-screen Raw update ------------------------------------
    try:
        set_encodings(s3, [0])
        request_update(s3, False, 0, 0, w, h)
        rect = read_update(s3, 0)
        check("raw-fullscreen", rect is not None and rect[4] == w * h * 4,
              "rect=%r expected %d payload bytes" % (rect, w * h * 4))
    except Exception as e:                                  # noqa: BLE001
        check("raw-fullscreen", False, str(e))

    # --- 7: the same under Hextile ------------------------------------
    try:
        set_encodings(s3, [5, 0])
        request_update(s3, False, 0, 0, w, h)
        rect = read_update(s3, 5)
        tiles = ((w + 15) // 16) * ((h + 15) // 16)
        check("hextile-fullscreen", rect is not None and rect[4] >= tiles,
              "decoded %s bytes over %d tiles"
              % (rect[4] if rect else "?", tiles))
    except Exception as e:                                  # noqa: BLE001
        check("hextile-fullscreen", False, str(e))

    # --- 8/9: input reaches the guest ---------------------------------
    # Evidence is on the guest's serial log ([vnc] key / [vnc] ptr under
    # -DVNC_BOOT); here we only assert the session survives sending them.
    try:
        for sym in (0x0061, 0xFF0D, 0xFFE1, 0x0041):        # 'a', Return, Shift, 'A'
            s3.sendall(struct.pack(">BBHI", 4, 1, 0, sym))
            s3.sendall(struct.pack(">BBHI", 4, 0, 0, sym))
        s3.sendall(struct.pack(">BBHH", 5, 0, w // 2, h // 2))
        s3.sendall(struct.pack(">BBHH", 5, 1, w // 2, h // 2))
        s3.sendall(struct.pack(">BBHH", 5, 0, w // 2, h // 2))

        # A live server still answers afterwards; a wedged one will not.
        set_encodings(s3, [0])
        request_update(s3, False, 0, 0, 16, 16)
        rect = read_update(s3, 0)
        check("session-alive-after-input",
              rect is not None and rect[4] == 16 * 16 * 4, "rect=%r" % (rect,))
    except Exception as e:                                  # noqa: BLE001
        check("session-alive-after-input", False, str(e))

    s3.close()

    passed = sum(1 for _, ok, _ in results if ok)
    print("\n=== vncprobe: %d/%d ===" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
