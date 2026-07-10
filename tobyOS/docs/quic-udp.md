# QUIC on the wire — UDP send + receive (HTTP/3 slices 4b + 4c)

Branch `quic-udp`, stacked on main (post the 3rd stage-13 merge).
tobyOS now completes a QUIC key-agreement exchange over the network:
it **sends** a real client Initial (4b) and **receives + processes**
the server's Initial (4c) — decrypting it, parsing the ServerHello,
and computing the same X25519 shared secret the server did. This
exercises the whole path (crypto → packet → frames → ClientHello →
UDP → server Initial → ServerHello → shared secret).

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

## Slice 4c — receive + ServerHello + shared secret
- **`quic_recv_hook()`** (`src/quic_conn.c`, registered in `udp.c` for
  the client's reply port under `-DQUIC_SEND_TEST`) captures the
  server's reply datagram into a buffer; `quic_udp_send_test()` polls
  for it with the DNS-style `rx_drain` + deadline loop after sending.
- On arrival it opens the server Initial with the **server** Initial
  keys (derived from the client's original DCID, `is_client=0`), walks
  the frames to the CRYPTO frame carrying the **ServerHello**, parses
  it (`quic_parse_server_hello` — server random, negotiated cipher,
  X25519 `key_share`), and computes the **X25519 shared secret** from
  which the handshake keys derive.

### Verified (QEMU SLIRP + host responder)
`quic_udp_responder.py` receives tobyOS's Initial, reads the client's
`key_share`, and replies with a real server Initial carrying a
ServerHello (its own X25519 key_share, cipher `TLS_AES_128_GCM_SHA256`)
protected with the server Initial keys.
- tobyOS serial: `received 140 bytes … server Initial decrypted (pn=0,
  94 payload bytes) … ServerHello OK: cipher=0x1301, x25519 shared
  secret 1b1a8c5ac83d88bf… HANDSHAKE KEYS DERIVABLE`.
- responder: `expected x25519 shared secret 1b1a8c5ac83d88bf…`.

The shared secrets **match exactly** — a genuine bidirectional QUIC
key-agreement over the wire. tobyOS decrypted a packet it received,
parsed the peer's ServerHello, and reached the same secret the peer
computed.

## What's next (HTTP/3 slices)
- **Slice 4d** — the rest of the handshake: derive Handshake-level
  QUIC keys from the shared secret, decrypt the server's Handshake
  packets (EncryptedExtensions / Certificate / CertificateVerify /
  Finished, reusing the stage-13H cert validation), send the client
  Finished, install 1-RTT keys. This is where the TLS message layer
  moves out of `tls.c` into a shared module.
- **Slice 5** — HTTP/3 HEADERS/DATA framing + QPACK over QUIC streams,
  into `http.c` behind an Alt-Svc / explicit-h3 probe with the h2/h1.1
  fallback ladder intact.

## v1 scope
Send the client Initial + receive/process the server Initial through
the shared-secret derivation. The encrypted Handshake flight
(Certificate/Finished) and 1-RTT are slice 4d; triggered at boot under
a build flag against a scripted responder (not yet a full live server).
