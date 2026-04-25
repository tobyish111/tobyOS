/* Service app template -- ticks once a second, logs to slog.
 * The kernel service supervisor (src/service.c) restarts this binary
 * according to the [service].restart policy in tobyapp.toml.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <toby/service.h>

static int tick(void *arg) {
    static unsigned ticks = 0;
    ticks++;
    char msg[64];
    snprintf(msg, sizeof(msg), "tick %u", ticks);
    toby_service_log(0 /* INFO */, msg);
    /* Returning 0 keeps the loop alive; non-zero exits with that rc. */
    (void)arg;
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    toby_service_log(0, "myservice starting");
    /* Sleep 1000 ms between ticks. The supervisor polls the exit
     * code if main returns; sleeping here yields to it cleanly. */
    return toby_service_run(1000, tick, NULL);
}
