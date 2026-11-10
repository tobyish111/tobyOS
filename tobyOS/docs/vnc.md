# VNC (RFB 3.8) remote desktop

tobyOS serves its composited desktop to any stock VNC client on **TCP 5900**,
and feeds remote keyboard and mouse input back into the real driver path.

- Server: [`src/vnc.c`](../src/vnc.c), header [`include/tobyos/vnc.h`](../include/tobyos/vnc.h)
- DES (for RFB authentication only): [`src/des.c`](../src/des.c)
- Password tool: `/bin/vncpasswd` ([`programs/user_vncpasswd/`](../programs/user_vncpasswd/))
- Gate: `bash logs/vnc.sh`

## Read this first: the session is NOT encrypted

RFB "VNC Authentication" (security type 2) protects the **password** with a
DES challenge-response, so the password never crosses the wire. It does
**not** encrypt the session. Once you are authenticated, every pixel and
every keystroke travels in the clear.

It is also a legacy scheme: the key is a single DES block, so the password is
capped at **8 characters** by the protocol itself. Longer input is truncated,
and `vncpasswd` says so rather than letting you believe otherwise.

Treat this as safe on a trusted LAN. Across anything else, tunnel it.

## Setting it up

The server refuses to listen until a password exists — there is no
"no authentication" mode and type 1 (None) is never advertised:

```
$ vncpasswd
Password:
Verify:
Password written to /data/vnc/vnc.passwd
The VNC server picks this up within a few seconds.
```

The password is stored under `/data` because `/etc` lives in the initrd
ramfs and is rebuilt from the image on every boot — the same reason the SSH
host key lives there. With no provisioned disk it falls back to
`/etc/vnc.passwd` and warns that the setting is temporary.

The file format is the reference `vncpasswd` format (the 8-byte password
DES-encrypted under the published fixed key), so files interchange with
other VNC tooling. That obfuscation is not security; the `0600` mode is what
keeps other local users out.

Then connect from anywhere on the network:

```
$ vncviewer <tobyos-ip>:5900       # or Remmina, RealVNC, macOS Screen Sharing
```

Under QEMU the run targets forward the port, so `localhost:5900` reaches the
guest.

## What is implemented

| Area | Status |
|---|---|
| RFB 3.8 handshake | yes |
| VNC Authentication (type 2), mandatory | yes |
| Brute-force cool-off (30s after 5 failures) | yes |
| Raw encoding | yes |
| Hextile encoding | yes — solid-tile and raw subencodings |
| DesktopSize pseudo-encoding | yes (resize is announced before pixels) |
| Pixel formats | 8/16/32 bpp true-colour, either endianness |
| Keyboard, incl. modifiers and shift synthesis | yes |
| Pointer, incl. wheel | yes |
| Clipboard, both directions | yes |

### How input works

Keys are injected as **PS/2 set-1 scancodes** through
`kbd_ps2_handle_scancode()` rather than as cooked characters. That reuses the
whole real path — shift/ctrl state, the E0 arrow cluster, and the evdev feed
that Linux processes read from `/dev/input/event0` — so a remote key is
indistinguishable from a local one.

Because clients disagree about whether to send `Shift_L` alongside a shifted
keysym, the server reconciles the driver's actual shift state against what
the keysym needs, synthesizing a shift press around the keystroke and
restoring it afterwards.

The pointer is placed **absolutely** via `gui_pointer_warp()`. RFB carries
absolute coordinates, and the ordinary relative path applies up to 5×
pointer acceleration, which would overshoot. `on_mouse_event()` was split so
both paths share all the hit-testing, drag, focus and dispatch logic.

### How updates work

`net_service_tick()` runs on the syscall-return path, so the server must
never block. A full-screen update is ~4 MB at 1280×800, and sending that with
the blocking `tcp_send()` would stall the machine mid-syscall. Instead pixels
are encoded into a bounded 64 KB TX window a row (Raw) or a tile (Hextile) at
a time and drained with `tcp_send_nb()`.

Damage comes from a **separate accumulator** in `gfx.c`
(`gfx_remote_damage_take()`). The compositor's own dirty rect is cleared by
`display.c` on every present, so a remote viewer sharing it would race the
local display and silently lose frames.

Measured on the gate at 1280×800: Raw sends 4,096,000 bytes; Hextile sends
213,104 for the same screen — about 19× smaller.

## Known limitations

- **One viewer at a time.** A second connection stays queued in the listener
  backlog until the first disconnects, rather than being refused with a
  message.
- **No CopyRect.** It would make window drags and scrolling much cheaper, but
  the compositor does not report where damage came *from*.
- **No ZRLE/Tight.** Both need a deflate compressor, which the tree does not
  have. Hextile is the ceiling for now.
- **Hextile does not emit sub-rectangles.** Tiles are either solid or raw, so
  a two-colour tile costs a full raw tile. Legal, just not optimal.
- **No view-only password.**
- **The remote cursor may not be drawn** when the display backend puts it on
  a hardware plane instead of the back buffer. In practice your own local
  pointer sits exactly where the remote one is, since positioning is
  absolute.

## The gate

```
bash logs/vnc.sh
```

Builds with `-DVNC_BOOT` (which seeds a known password), boots under QEMU
with 5900 forwarded, and runs `logs/vncprobe.py` — an independent RFB client
that computes its DES response **with openssl**, so a bug in `src/des.c`
cannot cancel against a matching bug in the test.

It checks both halves, and both matter:

- the **client** half proves the protocol, the authentication, and that
  complete framebuffer updates decode under Raw and Hextile;
- the **guest** half greps the serial log for the injected key and pointer
  events, because a server that accepted `KeyEvent`s and dropped them on the
  floor would still pass the client half.

`src/des.c` additionally carries `des_selftest()`, checked against published
FIPS 46-3 vectors and run before the server will start — a silently wrong DES
would otherwise present as "wrong password".
