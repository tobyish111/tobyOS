/* semaphore.h -- freestanding POSIX-semaphore shim for the vendored
 * openh264 decoder on tobyOS. libtoby has real pthreads but no
 * <semaphore.h>; the decoder only uses semaphores on the multi-threaded
 * path (threadCount > 1), which the tobyOS integration never enables
 * (single-threaded DecodeFrameNoDelay). These are therefore trivial
 * single-threaded stubs -- a counter with no blocking.
 *
 * This header is also where the few pthread attr/name functions the
 * openh264 thread layer references but that libtoby's <pthread.h> lacks
 * are declared, so libtoby stays untouched. All are no-ops (scheduling
 * hints / debug names) or thin wrappers -- never on the decode path. */

#ifndef TOBYOS_OPENH264_SEMAPHORE_SHIM_H
#define TOBYOS_OPENH264_SEMAPHORE_SHIM_H

#include <pthread.h>
#include <stdint.h>

typedef struct { volatile int value; } sem_t;

static inline int sem_init(sem_t *s, int pshared, unsigned int value) {
    (void)pshared; if (s) s->value = (int)value; return 0;
}
static inline int sem_destroy(sem_t *s) { (void)s; return 0; }
static inline int sem_post(sem_t *s) { if (s) s->value++; return 0; }
static inline int sem_wait(sem_t *s) { if (s && s->value > 0) s->value--; return 0; }
static inline int sem_trywait(sem_t *s) {
    if (s && s->value > 0) { s->value--; return 0; }
    return -1;
}
static inline int sem_getvalue(sem_t *s, int *v) {
    if (v) *v = s ? s->value : 0; return 0;
}
struct timespec;
static inline int sem_timedwait(sem_t *s, const struct timespec *t) {
    (void)t; return sem_wait(s);
}

/* pthread bits libtoby's <pthread.h> doesn't declare (thread-attr
 * init/scheduling hints + name). No-ops; never hit at threadCount 0. */
#ifndef PTHREAD_SCOPE_SYSTEM
#define PTHREAD_SCOPE_SYSTEM 0
#endif
#ifndef SCHED_FIFO
#define SCHED_FIFO 1
#endif
#ifndef SCHED_RR
#define SCHED_RR 2
#endif
static inline int pthread_attr_init(pthread_attr_t *a) { (void)a; return 0; }
static inline int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
static inline int pthread_attr_setschedpolicy(pthread_attr_t *a, int p) {
    (void)a; (void)p; return 0;
}
static inline int pthread_attr_setscope(pthread_attr_t *a, int s) {
    (void)a; (void)s; return 0;
}
static inline int pthread_setname_np(pthread_t t, const char *name) {
    (void)t; (void)name; return 0;
}
static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                         const struct timespec *t) {
    (void)t; return pthread_cond_wait(c, m);
}

#endif /* TOBYOS_OPENH264_SEMAPHORE_SHIM_H */
