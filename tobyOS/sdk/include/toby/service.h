/* toby/service.h -- helpers for SDK apps whose tobyapp.toml declares
 * `kind = "service"`.
 *
 * A "service" in tobyOS is just a long-lived ELF that the in-kernel
 * service supervisor (src/service.c) restarts according to its
 * restart policy. The kernel doesn't impose any extra protocol on
 * the binary -- a service is a normal user program that:
 *
 *   1. (optional) writes structured log records via tobylog_write
 *      so logview / serial.log can correlate its output with the
 *      service supervisor's restart counter,
 *   2. runs an event loop (typically blocking on a syscall like
 *      sys_nanosleep / sys_event / sys_poll),
 *   3. exits with a non-zero rc on failure -- the supervisor reads
 *      that rc and decides whether to restart, back off, or give up.
 *
 * This header bundles the few small conveniences that make writing
 * such a binary nicer: a log helper that fixes the subsystem tag,
 * a small "run forever, sleep N ms between iterations" macro, and
 * a few constants for restart policies that match the kernel's
 * SERVICE_RESTART_* enums. None of it is mandatory; you can write a
 * service entirely with libtoby's bare libc surface if you prefer.
 *
 * Header-only -- no toby_service.c is shipped. */

#ifndef TOBY_SERVICE_H
#define TOBY_SERVICE_H

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <tobyos_slog.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Restart policies as advertised in tobyapp.toml's [service] table.
 * Kernel-side ABI lives in src/service.c -- pkgbuild stamps these
 * into the package metadata so the supervisor can pick them up at
 * install time without reparsing the toml. */
#define TOBY_SERVICE_RESTART_ALWAYS     0
#define TOBY_SERVICE_RESTART_ONFAILURE  1
#define TOBY_SERVICE_RESTART_NEVER      2

/* Single-line structured log helper. Fixes the `sub` field at "svc"
 * so logview filters can pick out service activity at a glance.
 * Levels are the standard ABI_SLOG_LEVEL_* values. Returns 0 on
 * success, -1 with errno set on failure. */
static inline int toby_service_log(unsigned level, const char *msg) {
    return tobylog_write(level, "svc", msg);
}

/* Convenience: sleep `ms` milliseconds in a service loop. Wraps
 * nanosleep() so you don't have to compute timespecs yourself. */
static inline void toby_service_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = (long)(ms / 1000U);
    ts.tv_nsec = (long)((ms % 1000U) * 1000000UL);
    nanosleep(&ts, 0);
}

/* Generic "tick forever, every period_ms" loop. Inline so it costs
 * nothing if the optimiser decides to expand it. The body callback
 * returns 0 to keep going, non-zero to break. The exit code returned
 * is passed straight back to the supervisor. */
static inline int toby_service_run(unsigned period_ms,
                                   int (*tick)(void *), void *arg) {
    if (!tick) return 1;
    for (;;) {
        int rc = tick(arg);
        if (rc != 0) return rc;
        toby_service_sleep_ms(period_ms);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* TOBY_SERVICE_H */
