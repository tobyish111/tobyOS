/* vnc.h -- RFB 3.8 ("VNC") remote desktop server.
 *
 * Serves the composited desktop over TCP/5900 to any stock VNC client and
 * feeds KeyEvent/PointerEvent messages back into the real input path, so a
 * remote viewer drives the machine exactly as a local keyboard and mouse do.
 *
 * Authentication is MANDATORY: the server refuses to listen unless
 * /etc/vnc.passwd exists. RFB security type 2 ("VNC Authentication") is the
 * only type offered -- type 1 (None) is never advertised.
 */
#ifndef TOBYOS_VNC_H
#define TOBYOS_VNC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VNC_PORT         5900

/* Where the password lives. /etc is part of the initrd ramfs and is
 * rebuilt from the image on every boot, so a password stored there does
 * NOT survive a reboot -- the same reason the SSH host key lives under
 * /data. We prefer the persistent path and fall back to the conventional
 * one, which also lets a /etc/vnc.passwd baked into an image work. */
#define VNC_PASSWD_DIR   "/data/vnc"
#define VNC_PASSWD_PATH  "/data/vnc/vnc.passwd"
#define VNC_PASSWD_ALT   "/etc/vnc.passwd"

/* An RFB password is at most 8 bytes -- that is the DES key size, and the
 * protocol has no way to use more. Longer passwords are TRUNCATED, exactly
 * as every other VNC server does. */
#define VNC_PASSWD_LEN   8

/* Start listening. Safe to call repeatedly; only the first call binds.
 * Does nothing (and says so) when no password is configured. */
void vnc_init(void);

/* Cooperative, non-blocking service step. Driven from net_service_tick(),
 * which runs both from the idle loop and on syscall return. Must never
 * block: it is on the syscall-return path. */
void vnc_poll(void);

bool vnc_listening(void);
bool vnc_client_connected(void);

/* Write /etc/vnc.passwd from a plaintext password, using the same
 * fixed-key obfuscation the reference `vncpasswd` tool uses, so files are
 * interchangeable with it. Returns 0 on success or a negative VFS error. */
int vnc_set_password(const char *plain);

struct vnc_stats {
    uint32_t connects;
    uint32_t auth_ok;
    uint32_t auth_fail;
    uint32_t key_events;
    uint32_t pointer_events;
    uint32_t rects_sent;
    uint64_t bytes_sent;
    uint32_t raw_tiles;       /* hextile tiles sent as raw   */
    uint32_t solid_tiles;     /* hextile tiles sent as solid */
};
void vnc_get_stats(struct vnc_stats *out);

#endif /* TOBYOS_VNC_H */
