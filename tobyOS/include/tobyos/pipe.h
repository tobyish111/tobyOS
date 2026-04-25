/* pipe.h -- unidirectional kernel pipe (milestone 7).
 *
 * A pipe is a fixed-size circular byte buffer plus two "wait queues"
 * (singly-linked lists of struct proc, threaded via proc.next_wait):
 *
 *   wq_read   - readers blocked because count == 0 (and writers > 0)
 *   wq_write  - writers blocked because count == PIPE_BUF_SZ (and
 *               readers > 0)
 *
 * Lifetime is tracked by two independent counts of the number of
 * struct file aliases currently referring to each end:
 *
 *   readers   - decremented when a FILE_KIND_PIPE_R is closed
 *   writers   - decremented when a FILE_KIND_PIPE_W is closed
 *
 * When `readers` hits 0 we wake every blocked writer (their next write
 * will see no readers and return -EPIPE). When `writers` hits 0 we
 * wake every blocked reader (their next read will see an empty buffer
 * and return 0 = EOF). When BOTH hit 0 the pipe is freed.
 *
 * pipe_create() returns a fresh pair of files (PIPE_R, PIPE_W), each
 * with refcount 1, sharing one struct pipe with readers == writers == 1.
 * Cooperative scheduling means no explicit locking is required: every
 * code path here runs on the kernel stack with preemption off (the
 * kernel never preempts itself; user-mode preemption happens via SYSCALL
 * which always reaches us with IF=0).
 */

#ifndef TOBYOS_PIPE_H
#define TOBYOS_PIPE_H

#include <tobyos/types.h>

struct file;
struct proc;

#define PIPE_BUF_SZ 4096

struct pipe {
    uint8_t   buf[PIPE_BUF_SZ];
    uint32_t  head;      /* next byte to write into buf[head] */
    uint32_t  tail;      /* next byte to read from buf[tail] */
    uint32_t  count;     /* live bytes in the ring */

    int       readers;   /* live FILE_KIND_PIPE_R aliases */
    int       writers;   /* live FILE_KIND_PIPE_W aliases */

    struct proc *wq_read;   /* head of read-side block queue (proc.next_wait) */
    struct proc *wq_write;  /* head of write-side block queue */
};

/* Allocate a pipe + the two file aliases. On success returns 0 and
 * stuffs the read end into *out_r and the write end into *out_w. On
 * failure returns -1 and *out_r / *out_w are left untouched. */
int pipe_create(struct file **out_r, struct file **out_w);

/* Internal helpers used by file.c (kept in the header so file.c can
 * call them without a circular include via opaque pointers). */
long pipe_read (struct pipe *p, void *buf, size_t n);
long pipe_write(struct pipe *p, const void *buf, size_t n);
void pipe_close_reader(struct pipe *p);
void pipe_close_writer(struct pipe *p);

#endif /* TOBYOS_PIPE_H */
