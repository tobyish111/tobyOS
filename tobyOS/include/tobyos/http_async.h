/* http_async.h -- async HTTP transfers on a ring-0 kernel worker
 * (stage 12D). SYS_HTTP_START queues a transfer and returns a handle
 * without blocking; the worker proc (proc_create_kernel) runs the
 * existing synchronous engine (http_get_follow: DNS + TCP/TLS +
 * redirects + dechunk + gunzip + keep-alive) with tcp_yield_wait set,
 * so it shares its CPU while waiting. The app polls the handle from
 * its main loop and reads the body when DONE.
 *
 * The httpa_* entry points below are the kernel halves of
 * ABI_SYS_HTTP_START/POLL/READ/FINISH; syscall.c does the user-copy
 * and calls these with kernel buffers. All are called under the BKL
 * (syscall context) and mutate slot state only in short non-yielding
 * sections, so they interleave safely with the worker (which holds
 * the BKL except inside tcp waits / its idle yield). */

#ifndef TOBYOS_HTTP_ASYNC_H
#define TOBYOS_HTTP_ASYNC_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Queue a transfer. `url` is a kernel copy (NUL-terminated, < 512).
 * Returns a handle >= 0, or -1 when the slot table is full (after
 * reaping slots owned by exited procs). Spawns the worker lazily. */
int  httpa_start(const char *url, uint32_t max_body, int owner_pid);

/* Extended queue for fetch()/XHR: method / extra headers / body / flags
 * (all kernel copies; method<32B, headers<1KB, body<256KB). Same handle
 * semantics as httpa_start. NULL method == GET. */
int  httpa_start2(const char *url, const char *method, const char *headers,
                  const uint8_t *body, uint32_t body_len,
                  uint32_t max_body, unsigned flags, int owner_pid);

/* Copy the raw response header block of a DONE handle into `dst`.
 * Returns bytes copied (0 at/after end), or -1 on bad/foreign/undone. */
long httpa_hdrs(int h, int owner_pid, void *dst, uint32_t off, uint32_t len);

/* Fill *out with the handle's state. Returns 0, or -1 on a bad/foreign
 * handle. */
int  httpa_poll(int h, int owner_pid, struct abi_http_poll *out);

/* Copy body bytes [off, off+len) into `dst` (kernel buffer). Returns
 * bytes copied (0 at/after end), or -1 on a bad/foreign/undone handle. */
long httpa_read(int h, int owner_pid, void *dst, uint32_t off, uint32_t len);

/* Release the handle. Queued: freed immediately. Running: marked
 * cancelled (the worker discards the result and frees). Done/error:
 * body freed now. Returns 0, or -1 on a bad/foreign handle. */
int  httpa_finish(int h, int owner_pid);

#endif /* TOBYOS_HTTP_ASYNC_H */
