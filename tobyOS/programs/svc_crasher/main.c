/* programs/svc_crasher/main.c -- Milestone 28F: deterministic
 * crashing service used to exercise the supervisor.
 *
 * Behaviour: prints a single line and exits with rc=42 immediately.
 * Registered as a service in m28f_run_service_harness when SVCTEST
 * is on; the supervisor sees the bad exit, marks the service as
 * SERVICE_BACKOFF, waits the cooldown, retries -- after 5 consecutive
 * crashes it transitions to SERVICE_DISABLED and stops retrying. The
 * boot harness then spawns `services --boot` to read out the verdict.
 *
 * No syscalls beyond write+exit are needed. We deliberately keep
 * the binary tiny so the spawn-die-spawn loop completes quickly
 * even on slow QEMU runs. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stdout, "svc_crasher: hello, exiting non-zero\n");
    fflush(stdout);
    return 42;
}
