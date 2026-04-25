# Syscall reference

The full, frozen syscall ABI lives in `<tobyos/abi/abi.h>`. This
document organises it by area and points at the libtoby wrappers that
most apps should use instead of raw syscalls.

## Calling convention

The kernel uses the standard x86-64 syscall convention:

| reg | role |
|-----|------|
| `rax` | syscall number (in) / return value (out) |
| `rdi` | arg 1 |
| `rsi` | arg 2 |
| `rdx` | arg 3 |
| `r10` | arg 4 *(rcx is clobbered by SYSCALL; r10 replaces it)* |
| `r8`  | arg 5 |
| `r9`  | arg 6 |
| `rcx`, `r11` | clobbered by the kernel |

Negative return values in `[-4095, -1]` are `-ABI_E*` errno codes;
non-negative values are success.

libtoby's `__toby_check` translates the kernel convention into the
familiar POSIX shape: `errno` is set, `-1` is returned. All libtoby
wrappers do this for you.

## Process / lifecycle

| # | Syscall | libtoby wrapper |
|---|---------|------------------|
| 0 | `SYS_EXIT` | `_exit(int)` / `exit(int)` (with atexit) |
| 11 | `SYS_GETPID` | `getpid()` |
| 12 | `SYS_FORK` | `fork()` *(host fork-equivalent)* |
| 13 | `SYS_EXEC` | `execve(...)` |
| 14 | `SYS_WAITPID` | `waitpid(...)` |
| 15 | `SYS_GETTIMEOFDAY` | `gettimeofday()` |

## File I/O

| # | Syscall | libtoby wrapper |
|---|---------|------------------|
| 1 | `SYS_WRITE` | `write(fd, buf, n)` |
| 2 | `SYS_READ` | `read(fd, buf, n)` |
| 3 | `SYS_CLOSE` | `close(fd)` |
| 35 | `SYS_OPEN` | `open(path, flags, mode)` |
| 36 | `SYS_LSEEK` | `lseek(fd, off, whence)` |
| 37 | `SYS_STAT` | `stat(path, &st)` |
| 38 | `SYS_FSTAT` | `fstat(fd, &st)` |
| 41 | `SYS_UNLINK` | `unlink(path)` |
| 42 | `SYS_MKDIR` | `mkdir(path, mode)` |
| 44 | `SYS_GETCWD` | `getcwd(buf, n)` |
| 45 | `SYS_CHDIR` | `chdir(path)` |

Use **`<stdio.h>`** (`fopen`, `fread`, `fwrite`, `fprintf`) when you
just want to read/write text files; the libtoby implementation is
unbuffered but POSIX-shaped.

## Memory

| # | Syscall | libtoby wrapper |
|---|---------|------------------|
| 43 | `SYS_BRK` | `malloc/free` (built on top of brk) |
| 26 | `SYS_MMAP` | (raw -- no high-level wrapper) |

## GUI

| # | Syscall | libtoby wrapper |
|---|---------|------------------|
| 50 | `SYS_GUI_CREATE` | `tg_app_init` |
| 51 | `SYS_GUI_FILL` | `tg_run` (internal) |
| 52 | `SYS_GUI_TEXT` | `tg_run` (internal) |
| 53 | `SYS_GUI_FLIP` | `tg_run` (internal) |
| 54 | `SYS_GUI_POLL_EVENT` | `tg_run` (internal) |

The GUI toolkit owns these; you should never call them directly from
an application.

## Notifications  *(M31)*

| # | Syscall | libtoby wrapper |
|---|---------|------------------|
| 70 | `SYS_NOTIFY_POST` | `toby_notify_post(urgency, title, body)` |
| 71 | `SYS_NOTIFY_LIST` | `toby_notify_list(out, cap)` |
| 72 | `SYS_NOTIFY_DISMISS` | `toby_notify_dismiss(id)` (0 = all) |

Include `<tobyos_notify.h>`. The on-the-wire record is
`struct abi_notification` (200 bytes, frozen layout).

## Logging  *(M28A)*

| # | Syscall | libtoby wrapper |
|---|---------|------------------|
| 60 | `SYS_SLOG_READ` | `tobylog_read(out, cap, since_seq)` |
| 61 | `SYS_SLOG_WRITE` | `tobylog_write(level, sub, msg)` |
| 62 | `SYS_SLOG_STATS` | `tobylog_stats(&out)` |

Include `<tobyos_slog.h>`.

## Error codes

The full list is in `<tobyos/abi/abi.h>` (`ABI_E*`). The most common:

| Name | Meaning |
|------|---------|
| `ABI_EPERM` | operation not permitted |
| `ABI_ENOENT` | path/fd not found |
| `ABI_EBADF` | bad file descriptor |
| `ABI_EFAULT` | bad pointer from userspace |
| `ABI_EINVAL` | invalid argument |
| `ABI_ENOMEM` | out of memory |
| `ABI_EBUSY` | subsystem temporarily unavailable |
| `ABI_ENOSYS` | syscall not implemented |

libtoby maps these to the standard `errno.h` constants.

## Raw syscall escape hatch

If you must call a syscall directly (not recommended), the templates
show the freestanding link recipe; you can just inline:

```c
#include <tobyos/abi/abi.h>

static long raw_syscall1(long n, long a1) {
    long r;
    __asm__ volatile ("syscall"
                      : "=a"(r) : "0"(n), "D"(a1) : "rcx", "r11", "memory");
    return r;
}

long pid = raw_syscall1(ABI_SYS_GETPID, 0);
```
