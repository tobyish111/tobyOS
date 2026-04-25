# Writing a service

A "service" in tobyOS is a long-lived ELF that the in-kernel service
supervisor (`src/service.c`) restarts according to its restart
policy. The kernel imposes no extra protocol beyond:

1. The binary is started with `argc=0`, `argv=NULL` (no args today).
2. If `main` returns, the supervisor reads the exit code and decides
   whether to restart, back off, or give up (see [M31](services.md)
   notification flow).
3. The supervisor maintains a per-service crash counter; after
   `SERVICE_DISABLE_THRESHOLD` consecutive crashes the service is
   permanently disabled and a desktop notification is fired.

## Manifest

```toml
[app]
name = "myservice"
kind = "service"
version = "0.1.0"

[install]
prefix = "/data/apps/myservice"

[files]
"bin/myservice.elf" = "build/myservice.elf"

[service]
restart   = "on-failure"   # always | on-failure | never
autostart = true
```

## Skeleton

```c
#include <toby/service.h>

static int tick(void *arg) {
    (void)arg;
    /* one iteration of the service. Return 0 to keep going,
     * non-zero to exit with that as the return code. */
    return 0;
}

int main(void) {
    toby_service_log(0 /* INFO */, "myservice starting");
    return toby_service_run(1000 /* ms between ticks */, tick, NULL);
}
```

`toby_service_run` is just an inline `for (;;) { tick(); sleep(N); }`
loop. You're free to write your own event loop -- the only
requirement is to behave well as a long-lived process (don't
spin-wait, don't leak fds, don't grow the heap unboundedly).

## Restart policies

| Policy | Behaviour |
|--------|-----------|
| `always` | restart on any exit, including code 0 |
| `on-failure` | restart only when exit code is non-zero (default) |
| `never` | exit ends the service permanently |

Crash-loop containment kicks in for `always` and `on-failure`: after
~5 consecutive crashes within a short window the service is disabled
until the user runs `pkg remove` + `pkg install` to reset it.

## Logging

Use `toby_service_log(level, msg)` (which wraps `tobylog_write` with
`sub="svc"`) for anything you want to appear in `logview` and the
kernel serial log. Levels are 0=INFO, 1=NOTICE, 2=WARN, 3=ERR.

## Notifications

You can fire a desktop notification when something noteworthy happens
inside your service:

```c
#include <tobyos_notify.h>

if (something_bad) {
    toby_notify_post(TOBY_NOTIFY_WARN,
                     "myservice: degraded",
                     "DB connection retry #3");
}
```

This is what the kernel itself does for service crash events (see
`src/service.c`'s `apply_exit`).

## Inspecting a running service

From the shell:

```
services        # list registered services and their state
logview svc     # show only service-tagged log lines
```

`services` reads via `tobyos_svc.h`; you can call those APIs from your
own apps too.
