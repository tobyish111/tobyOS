/* libtoby/src/pthread.c -- POSIX threads implementation (Phase 1 M1.1).
 *
 * Backed by the kernel's SYS_THREAD_CREATE, SYS_THREAD_JOIN,
 * SYS_THREAD_EXIT, SYS_FUTEX, and SYS_SET_TLS syscalls. */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "libtoby_internal.h"

#define THREAD_STACK_SIZE  (64 * 1024)  /* 64 KiB per-thread user stack */

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#endif

/* ---- Internal thread trampoline ---- */

struct thread_start_info {
    void *(*func)(void *);
    void *arg;
};

static void __thread_trampoline(void *opaque) {
    struct thread_start_info info;
    memcpy(&info, opaque, sizeof(info));
    free(opaque);

    void *retval = info.func(info.arg);

    /* Exit the thread with retval encoded as exit code */
    toby_sc1(ABI_SYS_THREAD_EXIT, (long)(intptr_t)retval);
    __builtin_unreachable();
}

/* ---- Thread lifecycle ---- */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;

    /* Allocate a user stack for the new thread */
    void *stack = malloc(THREAD_STACK_SIZE);
    if (!stack) return ENOMEM;

    /* Pack the start info for the trampoline */
    struct thread_start_info *info = malloc(sizeof(*info));
    if (!info) { free(stack); return ENOMEM; }
    info->func = start_routine;
    info->arg  = arg;

    /* Stack grows downward; pass the top */
    uint64_t stack_top = (uint64_t)(uintptr_t)stack + THREAD_STACK_SIZE;
    /* Align to 16 bytes */
    stack_top &= ~(uint64_t)0xF;

    long tid = toby_sc4(ABI_SYS_THREAD_CREATE,
                        (long)(uintptr_t)__thread_trampoline,
                        (long)(uintptr_t)info,
                        (long)stack_top,
                        0 /* tls_base */);

    if (tid < 0) {
        free(info);
        free(stack);
        return EAGAIN;
    }

    if (thread) *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    int exit_code = 0;
    long r = toby_sc2(ABI_SYS_THREAD_JOIN, (long)thread, (long)&exit_code);
    if (r < 0) return ESRCH;
    if (retval) *retval = (void *)(intptr_t)exit_code;
    return 0;
}

int pthread_detach(pthread_t thread) {
    long r = toby_sc1(ABI_SYS_THREAD_DETACH, (long)thread);
    return (r < 0) ? ESRCH : 0;
}

void pthread_exit(void *retval) {
    toby_sc1(ABI_SYS_THREAD_EXIT, (long)(intptr_t)retval);
    __builtin_unreachable();
}

pthread_t pthread_self(void) {
    return (pthread_t)toby_sc0(ABI_SYS_GETTID);
}

/* ---- Mutex (futex-based) ---- */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    mutex->__lock = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    (void)mutex;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&mutex->__lock, &expected, 1,
                                        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;

        /* Contended -- sleep until woken */
        toby_sc3(ABI_SYS_FUTEX,
                 (long)&mutex->__lock, FUTEX_WAIT, 1);
    }
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&mutex->__lock, &expected, 1,
                                    0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    return EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    __atomic_store_n(&mutex->__lock, 0, __ATOMIC_RELEASE);
    /* Wake one waiter */
    toby_sc3(ABI_SYS_FUTEX,
             (long)&mutex->__lock, FUTEX_WAKE, 1);
    return 0;
}

/* ---- Condition variables (futex-based) ---- */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    cond->__seq   = 0;
    cond->__nwait = 0;
    cond->__mutex = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    uint32_t seq = __atomic_load_n(&cond->__seq, __ATOMIC_ACQUIRE);
    __atomic_fetch_add(&cond->__nwait, 1, __ATOMIC_RELAXED);

    pthread_mutex_unlock(mutex);

    /* Wait until seq changes (signal/broadcast bumps it) */
    toby_sc3(ABI_SYS_FUTEX,
             (long)&cond->__seq, FUTEX_WAIT, seq);

    __atomic_fetch_sub(&cond->__nwait, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(mutex);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    __atomic_fetch_add(&cond->__seq, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&cond->__nwait, __ATOMIC_RELAXED) > 0) {
        toby_sc3(ABI_SYS_FUTEX,
                 (long)&cond->__seq, FUTEX_WAKE, 1);
    }
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    __atomic_fetch_add(&cond->__seq, 1, __ATOMIC_RELEASE);
    uint32_t n = __atomic_load_n(&cond->__nwait, __ATOMIC_RELAXED);
    if (n > 0) {
        toby_sc3(ABI_SYS_FUTEX,
                 (long)&cond->__seq, FUTEX_WAKE, (long)n);
    }
    return 0;
}

/* ---- Thread-local storage (simplified) ---- */

#define PTHREAD_KEYS_MAX 64
static void *__tls_values[PTHREAD_KEYS_MAX];
static void (*__tls_destructors[PTHREAD_KEYS_MAX])(void *);
static int __tls_next_key = 0;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (__tls_next_key >= PTHREAD_KEYS_MAX) return EAGAIN;
    int k = __tls_next_key++;
    __tls_destructors[k] = destructor;
    __tls_values[k] = 0;
    if (key) *key = k;
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    if (key < 0 || key >= PTHREAD_KEYS_MAX) return EINVAL;
    __tls_destructors[key] = 0;
    __tls_values[key] = 0;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    if (key < 0 || key >= PTHREAD_KEYS_MAX) return 0;
    return __tls_values[key];
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key < 0 || key >= PTHREAD_KEYS_MAX) return EINVAL;
    __tls_values[key] = (void *)value;
    return 0;
}

/* ---- Once ---- */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    if (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == 1) return 0;

    int expected = 0;
    if (__atomic_compare_exchange_n(once_control, &expected, 2,
                                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init_routine();
        __atomic_store_n(once_control, 1, __ATOMIC_RELEASE);
        /* Wake anyone spinning on this */
        toby_sc3(ABI_SYS_FUTEX, (long)once_control, FUTEX_WAKE, 0x7FFFFFFF);
    } else {
        /* Another thread is running init; wait */
        while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) != 1) {
            toby_sc3(ABI_SYS_FUTEX, (long)once_control, FUTEX_WAIT, 2);
        }
    }
    return 0;
}
