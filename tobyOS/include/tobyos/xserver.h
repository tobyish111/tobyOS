/* xserver.h -- in-kernel fake X11 + MIT-SHM for Chromium Ozone (tier 2.5). */
#ifndef TOBYOS_XSERVER_H
#define TOBYOS_XSERVER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct sock;

/* Feed bytes the X client wrote. Returns bytes consumed (>= 0). */
long xserver_handle(struct sock *self, const void *buf, size_t n);

/* Continue decoding parked requests after the client drains RX replies. */
void xserver_pump(struct sock *self);

/* Called from poll/epoll readiness: drain parked work and, if the client
 * has been idle with an empty RX ring, inject a wake event so Ozone's
 * xcb_wait_for_event / UI pump cannot wedge forever with no traffic. */
void xserver_on_poll(struct sock *self);

/* Timer/idle tick: poke every fake-X client that is idle in wait_for_event
 * (recvmsg does not call on_poll, so without this the UI stays wedged). */
void xserver_tick(void);

/* Chromewin frame poll: fill dims/gen and optionally copy ARGB pixels.
 * If pixels==NULL, only metadata is filled. Returns 0 or -errno. */
long xframe_poll(uint32_t *w, uint32_t *h, uint32_t *stride,
                 uint32_t *gen, void *pixels, size_t pixels_cap);

/* Well-known SysV key for the primary Ozone window SHM ('TOBY'). */
#define XFRAME_SYSV_KEY  0x544F4259u

#endif
