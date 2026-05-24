/* pthread.h -- POSIX threads API for tobyOS (Phase 1 M1.1).
 *
 * Minimal pthreads implementation backed by tobyOS's thread/futex
 * syscalls. Provides: create/join/detach, mutexes, condition variables,
 * and thread-local storage keys. */

#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Types ---- */

typedef int pthread_t;
typedef struct { int __dummy; } pthread_attr_t;

typedef struct {
    volatile uint32_t __lock;   /* 0 = unlocked, 1 = locked */
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }

typedef struct { int __dummy; } pthread_mutexattr_t;

typedef struct {
    volatile uint32_t __seq;    /* sequence counter for wake */
    volatile uint32_t __nwait;  /* number of waiters */
    pthread_mutex_t  *__mutex;  /* associated mutex (unused in kernel) */
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER { 0, 0, 0 }

typedef struct { int __dummy; } pthread_condattr_t;

typedef int pthread_key_t;

typedef volatile int pthread_once_t;
#define PTHREAD_ONCE_INIT 0

/* ---- Thread lifecycle ---- */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);

/* ---- Mutex ---- */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

/* ---- Condition variables ---- */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

/* ---- Thread-local storage ---- */

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

/* ---- Once ---- */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

#ifdef __cplusplus
}
#endif

#endif /* _PTHREAD_H */
