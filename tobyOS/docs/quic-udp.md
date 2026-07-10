# QUIC on the wire — UDP send (HTTP/3 slice 4b)

Branch `quic-udp`, stacked on main (post the 3rd stage-13 merge).
tobyOS now sends a real QUIC client Initial packet **over the network**
— a live QUIC validator on the host decrypts and parses it. This turns
the slice-4a offline interop proof into an on-the-wire one, exercising
the whole outbound path (crypto → packet → frames → ClientHello → UDP).

## What shipped
- **`quic_udp_send_test()`** (`src/quic_conn.c`): builds a client
  Initial with a **random** connection ID + X25519 key + client random
  (real client behaviour), wraps the ClientHello (ALPN `h3` +
  transport parameters) in a CRYPTO frame, **pads the datagram to
  1200 bytes** (the RFC 9000 §8.1 anti-amplification minimum for a
  client Initial), and `udp_send()`s it to the SLIRP host at
  `10.0.2.2:4433`.
- **`src/kernel.c`**: calls it at boot, once `net_init()` reports the
  network up (right next to the existing `DHCPV6_SELFTEST` hook),
  gated behind `-DQUIC_SEND_TEST`.
- No new transport code — it reuses the kernel's existing
  `udp_send()` (the same primitive DNS/DHCP use).

## Verified (QEMU SLIRP + live host listener)
`quic_udp_listener.py` binds `udp/4433` on the host. tobyOS boots with
user-mode networking, gets a DHCP lease, and sends its Initial:
- tobyOS serial: `[quicudp] sent Initial 1203 bytes to 10.0.2.2:4433
  (dcid 44ee0374b676b8d2) OK`.
- host listener: `received 1203 bytes … PASS: live QUIC Initial from
  dcid=44ee0374b676b8d2 … decrypted; ClientHello 205B, ALPN=['h3']`.

The listener derives the Initial keys from the **received** packet's
DCID, removes header protection, authenticates the AES-128-GCM tag,
decrypts the payload, and parses the ClientHello with aioquic — all on
the datagram that actually crossed the virtual wire. The DCID matches
end to end (`44ee0374b676b8d2`), and the datagram is exactly 1200+
bytes as required.

## What's next (HTTP/3 slices)
- **Slice 4c** — receive: a `udp_recv` hook + poll to capture the
  server's Initial/Handshake datagrams in response, then the handshake
  state machine — parse ServerHello (Initial keys), derive Handshake
  keys, process EncryptedExtensions / Certificate / CertificateVerify
  / Finished (reusing the stage-13H cert validation) from Handshake
  packets, send the client Finished, install 1-RTT keys. This is where
  the TLS message builders/parsers move out of `tls.c` into a shared
  layer both TCP-TLS and QUIC drive.
- **Slice 5** — HTTP/3 HEADERS/DATA framing + QPACK over QUIC streams,
  into `http.c` behind an Alt-Svc / explicit-h3 probe with the
  h2/h1.1 fallback ladder intact.

## v1 scope
Send only: a single Initial to a fixed host/port, triggered at boot
under a build flag. Receiving and processing the server's response —
i.e. an actual handshake — is slice 4c.
