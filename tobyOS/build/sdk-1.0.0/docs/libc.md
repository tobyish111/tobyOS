# libtoby (libc surface) reference

`libtoby.a` ships a freestanding, POSIX-shape C library. It is not
glibc-compatible at the source level (smaller and simpler), but the
public APIs follow the POSIX-2017 signatures so most utility code
ports without changes.

## Headers

```
<assert.h>     <ctype.h>     <dirent.h>    <errno.h>    <fcntl.h>
<stdio.h>      <stdlib.h>    <string.h>    <time.h>     <unistd.h>
<sys/stat.h>   <sys/types.h> <sys/wait.h>
```

Plus the tobyOS-specific helpers:

```
<tobyos_slog.h>      structured kernel log access
<tobyos_hwinfo.h>    hardware inventory
<tobyos_notify.h>    desktop notifications  (M31)
<tobyos_svc.h>       service supervisor query API
<tobyos_stab.h>      stability self-tests
<tobyos_fscheck.h>   filesystem checker bindings
<tobyos_wdog.h>      watchdog timer
<tobyos_devtest.h>   device test harness
```

## What's missing

- **No floating-point conversions** in `printf` -- the kernel and
  in-tree apps don't use them, so they're not implemented yet.
- **No threading** -- tobyOS is single-threaded per process today.
- **No locale, no wide chars, no setjmp** -- not yet on the roadmap.
- **No `pthread_*`, `dlopen`, `signal` (in the POSIX sense)** --
  tobyOS does not implement these.

## Examples

### Read a file

```c
#include <stdio.h>

int main(void) {
    FILE *f = fopen("/data/etc/motd", "r");
    if (!f) return 1;
    char line[256];
    while (fgets(line, sizeof(line), f)) fputs(line, stdout);
    fclose(f);
    return 0;
}
```

### Write a file

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
    int fd = open("/data/scratch/hello.txt",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 1;
    static const char msg[] = "hello, persistent world\n";
    write(fd, msg, sizeof(msg) - 1);
    close(fd);
    return 0;
}
```

### List a directory

```c
#include <stdio.h>
#include <dirent.h>

int main(void) {
    DIR *d = opendir("/data");
    if (!d) return 1;
    struct dirent *de;
    while ((de = readdir(d))) printf("%s\n", de->d_name);
    closedir(d);
    return 0;
}
```

### Allocate dynamically

```c
#include <stdlib.h>
#include <string.h>

int main(void) {
    char *buf = malloc(64);
    if (!buf) return 1;
    strcpy(buf, "hello");
    free(buf);
    return 0;
}
```

`malloc` is built on top of `SYS_BRK`. There is no `mmap`-backed
heap; large allocations come straight off the program break. See
`libtoby/src/init.c` for the heap initialisation.

### Sleep

```c
#include <time.h>

int main(void) {
    struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
    return 0;
}
```

### Read the slog ring

```c
#include <stdio.h>
#include <tobyos_slog.h>

int main(void) {
    struct abi_slog_record buf[16];
    int n = tobylog_read(buf, 16, 0);
    if (n < 0) { perror("slog_read"); return 1; }
    for (int i = 0; i < n; i++) {
        printf("[%llu] %u/%s: %s\n",
               (unsigned long long)buf[i].seq,
               buf[i].level, buf[i].sub, buf[i].msg);
    }
    return 0;
}
```
