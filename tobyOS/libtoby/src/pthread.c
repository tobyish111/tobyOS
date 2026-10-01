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

/* ---- Thread stacks, and who owns them -------------------------------
 *
 * pthread_create() mallocs the new thread's user stack, and NOTHING freed
 * it. Every thread leaked 64 KiB for the life of the process, so a program
 * that creates threads in a loop dies of heap exhaustion: measured on real
 * hardware 2026-09-11, /bin/tjoinchurn got 366 rounds into a 2000-round run
 * before pthread_create started returning ENOMEM -- 366 x 64 KiB of stacks
 * that nobody owned any more.
 *
 * The thread cannot free its own stack: it is still running on it. So the
 * JOINER frees it, which is the one moment the stack is provably idle --
 * thread_join has already reaped the thread by then. That needs a tid ->
 * stack mapping, because pthread_join is handed only a tid.
 *
 * A DETACHED thread has no joiner, so its stack is swept later instead:
 * pthread_create walks the table and frees any detached entry whose thread
 * is gone, using kill(tid, 0) -- the POSIX liveness idiom -- as the test.
 * The sweep runs at create time because that is exactly when the memory is
 * about to be wanted. A detached thread that never exits keeps its stack,
 * which is correct: it is still standing on it. */

#define PT_MAX_TRACKED  256

struct pt_stack {
    int   in_use;
    void *stack;
    int   tid;
    int   detached;     /* nobody will join it; the sweep owns it */
    int   exited;       /* the thread itself set this on its way out */
};

static struct pt_stack g_pt_stacks[PT_MAX_TRACKED];

/* WHY SLOTS AND NOT TIDS.
 *
 * The first version of this asked kill(tid, 0) whether a detached thread
 * was still alive. That is the POSIX idiom and it is WRONG HERE, because
 * tids are recycled aggressively -- a real run shows tid 8 reused dozens
 * of times a second. A dead thread's tid gets handed to a live one, the
 * test answers "still running", the entry is never reclaimed, the table
 * fills, and new stacks stop being tracked at all. Measured on hardware:
 * it got 23x further than the unfixed leak and still died of ENOMEM, with
 * the sweep's kill() calls making up most of 67511 syscalls.
 *
 * So nothing is keyed on a tid that a thread might not own any more. Each
 * thread is handed its own SLOT INDEX at creation and marks that slot on
 * its way out -- an index nobody else can be given while it is in use. */

static int pt_alloc_slot(void *stack) {
    for (int i = 0; i < PT_MAX_TRACKED; i++) {
        if (!g_pt_stacks[i].in_use) {
            g_pt_stacks[i].in_use   = 1;
            g_pt_stacks[i].stack    = stack;
            g_pt_stacks[i].tid      = 0;
            g_pt_stacks[i].detached = 0;
            g_pt_stacks[i].exited   = 0;
            return i;
        }
    }
    return -1;                       /* table full; caller declines to create */
}

static void pt_free_slot(int i) {
    free(g_pt_stacks[i].stack);
    g_pt_stacks[i].stack  = 0;
    g_pt_stacks[i].in_use = 0;
    g_pt_stacks[i].tid = g_pt_stacks[i].detached = g_pt_stacks[i].exited = 0;
}

/* The joinable thread `tid` has been reaped: hand its stack back. A
 * DETACHED entry is skipped even on a tid match, because its tid may since
 * have been reissued to the thread actually being joined. */
static void pt_release_joined(int tid) {
    for (int i = 0; i < PT_MAX_TRACKED; i++)
        if (g_pt_stacks[i].in_use && !g_pt_stacks[i].detached &&
            g_pt_stacks[i].tid == tid) { pt_free_slot(i); return; }
}

static void pt_mark_detached(int tid) {
    for (int i = 0; i < PT_MAX_TRACKED; i++)
        if (g_pt_stacks[i].in_use && !g_pt_stacks[i].detached &&
            g_pt_stacks[i].tid == tid) { g_pt_stacks[i].detached = 1; return; }
}

/* Reclaim detached threads that have left. No syscall: the thread set
 * `exited` itself, immediately before the exit that never returns. */
static void pt_sweep_detached(void) {
    for (int i = 0; i < PT_MAX_TRACKED; i++)
        if (g_pt_stacks[i].in_use && g_pt_stacks[i].detached &&
            g_pt_stacks[i].exited) pt_free_slot(i);
}

/* ---- Internal thread trampoline ---- */

struct thread_start_info {
    void *(*func)(void *);
    void *arg;
    int   slot;                      /* our entry in g_pt_stacks */
};

static void __thread_trampoline(void *opaque) {
    struct thread_start_info info;
    memcpy(&info, opaque, sizeof(info));
    free(opaque);

    void *retval = info.func(info.arg);

    /* WE ARE LEAVING: say so, then go. The store lands in the table, not
     * on this stack, and the very next thing is an exit that never
     * returns -- so from the store onward nothing here touches the stack
     * again and a sweeper may reclaim it.
     *
     * Residual window, stated rather than hidden: between the store and
     * the syscall the compiler could in principle spill to this stack.
     * Closing it properly needs the KERNEL to clear a user word after the
     * thread is off its stack -- which the Linux side of this kernel
     * already does (clear_child_tid, see proc.c) and the native ABI has
     * no equivalent of. That is the real fix if this ever bites. */
    if (info.slot >= 0) g_pt_stacks[info.slot].exited = 1;
    toby_sc1(ABI_SYS_THREAD_EXIT, (long)(intptr_t)retval);
    __builtin_unreachable();
}

/* ---- Thread lifecycle ---- */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;

    /* From now on stdio locks its streams (K5: they are buffered, and two
     * threads printing into one buffer would corrupt it). */
    __toby_stdio_threaded = 1;

    /* Reclaim any detached thread that has exited since the last create;
     * this is the moment its memory is about to be needed. */
    pt_sweep_detached();

    /* Allocate a user stack for the new thread */
    void *stack = malloc(THREAD_STACK_SIZE);
    if (!stack) return ENOMEM;

    int slot = pt_alloc_slot(stack);
    if (slot < 0) { free(stack); return EAGAIN; }   /* table full */

    /* Pack the start info for the trampoline */
    struct thread_start_info *info = malloc(sizeof(*info));
    if (!info) { pt_free_slot(slot); return ENOMEM; }
    info->func = start_routine;
    info->arg  = arg;
    info->slot = slot;

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
        pt_free_slot(slot);
        return EAGAIN;
    }

    g_pt_stacks[slot].tid = (int)tid;
    if (thread) *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    int exit_code = 0;
    long r = toby_sc2(ABI_SYS_THREAD_JOIN, (long)thread, (long)&exit_code);
    if (r < 0) return ESRCH;
    /* The join has reaped it, so the stack is provably idle now -- this is
     * the one safe moment to hand it back. See the table above. */
    pt_release_joined((int)thread);
    if (retval) *retval = (void *)(intptr_t)exit_code;
    return 0;
}

int pthread_detach(pthread_t thread) {
    long r = toby_sc1(ABI_SYS_THREAD_DETACH, (long)thread);
    if (r < 0) return ESRCH;
    /* No joiner will ever come, and the thread may still be running on
     * this stack, so it cannot be freed here. Mark it for the sweep in
     * pthread_create, which frees it once kill(tid, 0) says it is gone. */
    pt_mark_detached((int)thread);
    return 0;
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
