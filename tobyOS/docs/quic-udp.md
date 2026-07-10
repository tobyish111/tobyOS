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

## Slice 4d — Handshake keys + decrypt the Handshake flight
- **`quic_open_long()`** (`src/quic_packet.c`) generalizes packet-open
  to Initial *and* Handshake long headers (Handshake has no token
  field) and reports each packet's on-wire length, so the caller can
  walk **coalesced** packets in one datagram (`quic_open_initial` is
  now a thin wrapper).
- After the shared secret, `quic_udp_send_test()` runs the RFC 8446
  §7.1 key schedule over the transcript `SHA-256(ClientHello ||
  ServerHello)` — early → derived → handshake secret → server
  handshake traffic secret — then derives the **Handshake-level QUIC
  keys** (`quic key`/`iv`/`hp`) via the `tls13` HKDF, opens the
  coalesced server **Handshake** packet with them, and parses the
  first handshake message out of its CRYPTO frame.

### Verified (QEMU SLIRP + responder coalescing Initial + Handshake)
The responder now replies with a coalesced datagram: server
Initial(ServerHello) + Handshake(EncryptedExtensions), the Handshake
packet protected with the handshake keys it derives from the shared
secret + transcript.
- tobyOS serial: `ServerHello OK … shared secret 452ccadfe1465a2a…`,
  `derived server Handshake keys (key 5834913d6f62…)`, `Handshake
  packet DECRYPTED (pn=0): first msg type=8 (EncryptedExtensions) --
  HANDSHAKE FLIGHT READABLE`.
- responder: `expected shared secret 452ccadfe1465a2a…`, `expected
  server Handshake key 5834913d6f62…`.

Both the shared secret **and** the Handshake keys match, and tobyOS
decrypts a real Handshake-level packet and reads the EncryptedExtensions
— the TLS 1.3 key schedule works across two QUIC encryption levels.

## Slice 4e — handshake completion (server Finished + 1-RTT)
- `quic_udp_send_test()` now reassembles the server's Handshake flight
  from the CRYPTO frame, walks its messages
  (**EncryptedExtensions / Certificate / CertificateVerify /
  Finished**), continues the transcript hash through
  EE+Cert+CertVerify, and **verifies the server Finished MAC**:
  `HMAC(finished_key, SHA-256(CH‖SH‖EE‖Cert‖CertVerify))`, with
  `finished_key = Expand-Label(server handshake secret, "finished")`.
  Matching this proves both sides agree on the *entire* handshake
  transcript and key schedule.
- It then derives + **installs the 1-RTT (application) keys**: master
  secret → client application traffic secret → the `quic key/iv/hp`
  used to protect 1-RTT packets.

### Verified (QEMU SLIRP + responder full flight)
The responder replies with server Initial(ServerHello) coalesced with
Handshake(EE + Certificate + CertificateVerify + Finished), the server
Finished computed over the correct transcript. tobyOS serial:
`shared secret a4029dd573fa0f28…`, `derived server Handshake keys (key
fb0e20ac07cd…)`, `flight: EE=1 Certificate=1 CertVerify=1 Finished=1`,
`server Finished VERIFIED -- transcript + key schedule agree
end-to-end`, `1-RTT keys installed (client app key 21301ca387c8…) --
QUIC HANDSHAKE COMPLETE (crypto)`. The responder's server-Finished MAC
was cross-checked against an independent RFC 8446 reference.

## What's next (HTTP/3 slices)
- **Slice 4f** — cert authentication + client Finished: validate the
  Certificate chain and CertificateVerify signature (reusing the
  stage-13H BearSSL path, which requires lifting `tls_validate_chain`
  / `tls_verify_cv` out of `tls.c` into a shared module) and send the
  client Finished. Against a full live QUIC server with a real cert,
  not the scripted responder.
- **Slice 5** — HTTP/3 HEADERS/DATA framing + QPACK over QUIC 1-RTT
  streams, into `http.c` behind an Alt-Svc / explicit-h3 probe with
  the h2/h1.1 fallback ladder intact.

## v1 scope
Completes the QUIC handshake *cryptographically* — key agreement,
Handshake keys, server Finished verification, and 1-RTT key install —
against a scripted responder. Certificate *authentication* (chain +
CertVerify signature) and the client Finished send are slice 4f; a full
live QUIC server is where that gets exercised. Triggered at boot under
a build flag.
