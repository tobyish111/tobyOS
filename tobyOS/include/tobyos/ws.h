/* ws.h -- WebSocket client (RFC 6455) on a ring-0 kernel worker
 * (stage 13). SYS_WS_OPEN queues the connect + HTTP Upgrade handshake
 * and returns a handle without blocking; the worker proc (the same
 * proc_create_kernel + tcp_yield_wait model as http_async.c) owns the
 * transport for the connection's whole life -- it performs the
 * handshake, decodes incoming frames into a per-handle message queue,
 * auto-answers pings, reassembles fragmented messages, and drains an
 * outgoing queue of pre-masked frames. wss:// runs over the existing
 * TLS 1.3 client (tls.c), certificate validation included.
 *
 * The ws_* entry points below are the kernel halves of
 * ABI_SYS_WS_OPEN/POLL/RECV/SEND/CLOSE; syscall.c does the user-copy
 * and calls these with kernel buffers. All run under the BKL in short
 * non-yielding sections and touch only the slot's queues -- the
 * transport itself is worker-only, so a syscall can never race the
 * worker inside a TCP/TLS wait. */

#ifndef TOBYOS_WS_H
#define TOBYOS_WS_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Queue a connection. `url` is a kernel copy (NUL-terminated, < 512,
 * ws:// or wss://). Returns a handle >= 0, or -1 when the slot table
 * is full / the URL is unusable. Spawns the worker lazily. */
int  ws_open(const char *url, int owner_pid);

/* Fill *out with the handle's state + head-of-queue message info.
 * Returns 0, or -1 on a bad/foreign handle. */
int  ws_poll(int h, int owner_pid, struct abi_ws_poll *out);

/* Pop the head message into `dst` (kernel buffer, cap bytes; a longer
 * message is truncated). Returns its byte length (0-length messages
 * are legal), or -1 when nothing is queued / bad handle. */
long ws_recv(int h, int owner_pid, void *dst, uint32_t cap);

/* Queue one outgoing message (op = ABI_WS_OP_TEXT/BINARY). The worker
 * masks + frames + sends it. Returns 0, or -1 (bad handle / not open /
 * send queue full). */
int  ws_send(int h, int owner_pid, const void *msg, uint32_t len, int op);

/* Release the handle. On an open connection this queues a Close frame
 * (with `code`, 0 = 1000) for the worker, which finishes the closing
 * handshake and frees the slot. Always detaches the app immediately.
 * Returns 0, or -1 on a bad/foreign handle. */
int  ws_close(int h, int owner_pid, uint16_t code);

#endif /* TOBYOS_WS_H */
