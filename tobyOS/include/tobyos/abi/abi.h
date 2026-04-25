/* abi.h -- THE canonical tobyOS userspace ABI.
 *
 * Milestone 25A: this header is the single source of truth for the
 * kernel/userspace contract. Every syscall number, every error code,
 * and every on-the-wire struct that crosses the ring-boundary lives
 * here. The kernel includes it via <tobyos/syscall.h>; the libc
 * (milestone 25B) ships a copy under libc/include/tobyos/abi/abi.h
 * and the two MUST stay byte-identical.
 *
 * Stability promise (effective from milestone 25A):
 *   - Numbers in the FROZEN ranges below NEVER change meaning. New
 *     calls go in unused slots only; existing slots keep their
 *     argument order, semantics, and return convention forever.
 *   - On-the-wire structs in the FROZEN ranges keep their layout.
 *     New fields go on the end behind versioned macros only.
 *   - Error codes are negative integers. Adding new ones is safe;
 *     repurposing an existing value is not.
 *
 * This header is freestanding-C-clean: no kernel-only types, no
 * heavy includes, no header-only inline that depends on libc. A
 * userspace program with nothing but a freestanding compiler must
 * be able to consume it.
 *
 * ============================================================
 *  CALLING CONVENTION (x86_64 SYSCALL/SYSRET, frozen)
 * ============================================================
 *
 *   rax  syscall number  (in)    return value           (out)
 *   rdi  arg1
 *   rsi  arg2
 *   rdx  arg3
 *   r10  arg4   (rcx is clobbered by SYSCALL; r10 is its replacement)
 *   r8   arg5
 *   r9   arg6
 *   rcx  user RIP after syscall  (clobbered)
 *   r11  user RFLAGS             (clobbered)
 *   all other GP regs preserved across the call.
 *
 * Return convention:
 *   rax >= 0      : success; value is the syscall's natural result
 *                   (bytes transferred, fd allocated, pid, ...).
 *   rax == -1     : legacy "any error" sentinel, used by the milestone
 *                   <25 syscalls. New code SHOULD prefer the explicit
 *                   -ETOBY_* codes below.
 *   rax in [-4095, -1]
 *                 : negated ETOBY_* error code. Userspace converts via
 *                   `if ((unsigned long)rv > -4096UL) errno = -rv`.
 *
 * Pointer / buffer rules:
 *   - All user pointers are validated to live entirely in the user
 *     half (< 0x0000_8000_0000_0000) and not wrap.
 *   - String args are NUL-terminated and capped at the per-call
 *     limit documented for that syscall (path: ABI_PATH_MAX, etc).
 */

#ifndef TOBYOS_ABI_ABI_H
#define TOBYOS_ABI_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ABI version stamp ----------------------------------------- *
 *
 * Bumped only when a backwards-incompatible change lands. Userspace
 * can fetch it via SYS_ABI_VERSION (call number ABI_SYS_ABI_VERSION). */
#define TOBY_ABI_VERSION   1u

/* ============================================================
 *  Numeric limits
 * ============================================================ */

/* Maximum path length passed across a syscall (incl. trailing NUL). */
#define ABI_PATH_MAX        256

/* Maximum read/write request size honoured by the kernel per call.
 * Larger requests are silently clipped to this. */
#define ABI_RW_MAX          65536u

/* Maximum argv string and environment string length passed to spawn. */
#define ABI_ARG_MAX         512

/* Maximum number of argv / envp entries passed to spawn. */
#define ABI_ARGV_MAX        64
#define ABI_ENVP_MAX        64

/* ============================================================
 *  Error codes (negative; userspace negates back to ABI_E*).
 * ============================================================ */

#define ABI_OK               0
#define ABI_EPERM            1   /* operation not permitted */
#define ABI_ENOENT           2   /* no such file or directory */
#define ABI_EIO              5   /* I/O error from underlying device */
#define ABI_E2BIG            7   /* arg/env list too long */
#define ABI_EBADF            9   /* bad file descriptor */
#define ABI_ECHILD          10   /* no child processes */
#define ABI_ENOMEM          12   /* out of memory */
#define ABI_EACCES          13   /* permission denied */
#define ABI_EFAULT          14   /* bad address */
#define ABI_EBUSY           16   /* resource busy */
#define ABI_EEXIST          17   /* file exists */
#define ABI_ENOTDIR         20   /* not a directory */
#define ABI_EISDIR          21   /* is a directory */
#define ABI_EINVAL          22   /* invalid argument */
#define ABI_EMFILE          24   /* too many open files (per-proc cap) */
#define ABI_ENOSPC          28   /* no space left on device */
#define ABI_EROFS           30   /* read-only filesystem */
#define ABI_EPIPE           32   /* broken pipe */
#define ABI_ERANGE          34   /* result out of range */
#define ABI_ENAMETOOLONG    36   /* file name too long */
#define ABI_ENOSYS          38   /* function not implemented */

/* ============================================================
 *  Syscall numbers
 * ============================================================
 *
 *  --- Range 0..30: FROZEN milestones <=24 (do not renumber) ---  */

/* Core process / I/O (milestone 4..7). */
#define ABI_SYS_EXIT             0
#define ABI_SYS_WRITE            1
#define ABI_SYS_READ             2
#define ABI_SYS_PIPE             3
#define ABI_SYS_CLOSE            4
#define ABI_SYS_YIELD            5

/* Networking (milestone 9). */
#define ABI_SYS_SOCKET           6
#define ABI_SYS_BIND             7
#define ABI_SYS_SENDTO           8
#define ABI_SYS_RECVFROM         9

/* GUI (milestone 10). */
#define ABI_SYS_GUI_CREATE      10
#define ABI_SYS_GUI_FILL        11
#define ABI_SYS_GUI_TEXT        12
#define ABI_SYS_GUI_FLIP        13
#define ABI_SYS_GUI_POLL_EVENT  14

/* Term + VFS + async exec (milestone 13). */
#define ABI_SYS_TERM_OPEN       15
#define ABI_SYS_TERM_WRITE      16
#define ABI_SYS_TERM_READ       17
#define ABI_SYS_FS_READDIR      18
#define ABI_SYS_FS_READFILE     19
#define ABI_SYS_EXEC            20  /* desktop/launcher async exec */

/* Settings + session (milestone 14). */
#define ABI_SYS_SETTING_GET     21
#define ABI_SYS_SETTING_SET     22
#define ABI_SYS_LOGIN           23
#define ABI_SYS_LOGOUT          24
#define ABI_SYS_SESSION_INFO    25

/* User identity (milestone 15). */
#define ABI_SYS_GETUID          26
#define ABI_SYS_GETGID          27
#define ABI_SYS_USERNAME        28
#define ABI_SYS_CHMOD           29
#define ABI_SYS_CHOWN           30

/*  --- Range 31..63: NEW in milestone 25A. libc-shape calls.  */

/* Process introspection / lifecycle. */
#define ABI_SYS_GETPID          31  /* () -> int pid */
#define ABI_SYS_GETPPID         32  /* () -> int ppid (0 if pid 0 or none) */
#define ABI_SYS_SPAWN           33  /* (const struct abi_spawn_req *)
                                       -> int pid_of_child or -ABI_E* */
#define ABI_SYS_WAITPID         34  /* (int pid, int *status_out, int flags)
                                       -> reaped pid or -ABI_E* */

/* Filesystem (POSIX-shape). */
#define ABI_SYS_OPEN            35  /* (const char *path, int flags, int mode)
                                       -> fd or -ABI_E*  (flags = ABI_O_*) */
#define ABI_SYS_LSEEK           36  /* (int fd, int64_t off, int whence)
                                       -> new pos or -ABI_E* */
#define ABI_SYS_STAT            37  /* (const char *path, struct abi_stat *out)
                                       -> 0 or -ABI_E* */
#define ABI_SYS_FSTAT           38  /* (int fd,           struct abi_stat *out)
                                       -> 0 or -ABI_E* */
#define ABI_SYS_DUP             39  /* (int oldfd) -> newfd or -ABI_E* */
#define ABI_SYS_DUP2            40  /* (int oldfd, int newfd)
                                       -> newfd or -ABI_E* */
#define ABI_SYS_UNLINK          41  /* (const char *path) -> 0 or -ABI_E* */
#define ABI_SYS_MKDIR           42  /* (const char *path, int mode)
                                       -> 0 or -ABI_E* */

/* Memory. */
#define ABI_SYS_BRK             43  /* (uintptr_t new_brk)
                                       -> current brk after request.
                                       new_brk == 0 -> just query. */

/* Environment + cwd. */
#define ABI_SYS_GETCWD          44  /* (char *out, size_t cap)
                                       -> bytes written excl NUL or -ABI_E* */
#define ABI_SYS_CHDIR           45  /* (const char *path) -> 0 or -ABI_E* */
#define ABI_SYS_GETENV          46  /* (const char *name, char *out, size_t cap)
                                       -> bytes written excl NUL, 0 if unset,
                                          or -ABI_E* */

/* Time. */
#define ABI_SYS_NANOSLEEP       47  /* (uint64_t nanoseconds) -> 0 */
#define ABI_SYS_CLOCK_MS        48  /* () -> milliseconds since boot */

/* Diagnostics. */
#define ABI_SYS_ABI_VERSION     49  /* () -> TOBY_ABI_VERSION */

/*  --- Range 50..63: NEW in milestone 25D. Dynamic loader. */

/* Load an ET_DYN ELF (a shared object) from `path` into the calling
 * process's address space at `base`, then write back layout info the
 * userspace dynamic linker needs to relocate and resolve symbols
 * against it.
 *
 * Constraints:
 *   - `base` must be page-aligned, in the user half, and not overlap
 *     any existing user mapping.
 *   - The file must be ET_DYN. ET_EXEC is not loadable this way.
 *   - On success the library's PT_LOAD pages are mapped with their
 *     final permissions (R/X for .text, R for .rodata, RW for .data
 *     /.bss). The dynamic linker can still write to RW segments to
 *     apply relocations.
 *
 * This is intentionally the SINGLE new kernel surface for M25D: it
 * lets ld-toby.so do all symbol/relocation work in user mode while
 * keeping byte-into-page page-table work in the kernel where it
 * already lives. Adding a generic mmap()+mprotect() pair is a
 * possible follow-up but explicitly out of scope here.
 */
#define ABI_SYS_DLOAD           50  /* (const char *path, uint64_t base,
                                       struct abi_dlmap_info *out)
                                       -> 0 or -ABI_E* */

/*  --- Range 51..63: NEW in milestone 26A. Peripheral test harness. */

/* SYS_DEV_LIST: enumerate kernel-known devices into a flat user buffer.
 *
 *   args:  (struct abi_dev_info *out, uint32_t cap, uint32_t type_mask)
 *   ret:   number of records written (>= 0), or -ABI_E*.
 *
 * The kernel walks every introspected subsystem (PCI / USB / block /
 * input / audio / battery / hub) and emits one fixed-size record per
 * device. `type_mask` is the OR of ABI_DEVT_BUS_* bits to filter; pass
 * 0 to mean "all". `cap` caps the number of records returned -- if
 * more devices exist than fit, the truncation is silent and the caller
 * sees only the first `cap` (records are stable-ordered: PCI by
 * (bus,slot,fn), then USB by (port,slot), then BLK by registration
 * order, then virtual buses).
 *
 * The struct layout is FROZEN; new fields go on the end behind a
 * version field on the next ABI bump.
 */
#define ABI_SYS_DEV_LIST        51

/* SYS_DEV_TEST: run a registered driver/subsystem self-test by name.
 *
 *   args:  (const char *name, char *msg_out, uint32_t msg_cap)
 *   ret:    0           = PASS
 *           ABI_DEVT_SKIP (= +1) = skipped (e.g. hardware not present)
 *          -ABI_E*       = FAIL (and msg_out has a one-line reason)
 *
 * `msg_out` is filled with a NUL-terminated short diagnostic string
 * (at most ABI_DEVT_MSG_MAX). On a clean PASS the message may be
 * empty; on SKIP/FAIL it should explain why in human English. */
#define ABI_SYS_DEV_TEST        52

/* SYS_HOT_DRAIN: drain pending hot-plug events into a user buffer.
 *
 *   args:  (struct abi_hot_event *out, uint32_t cap)
 *   ret:   number of records written (>= 0), or -ABI_E*.
 *
 * Returns up to `cap` events from the kernel's per-boot ring buffer.
 * Each event represents one device-state transition (attach, detach,
 * error). Events are FIFO with bounded backpressure: the ring caps at
 * ABI_DEVT_HOT_RING entries, and an overflow counter is tucked in the
 * `dropped` field of the OLDEST returned record so userland can detect
 * lost events without polling extra syscalls.
 *
 * Calling this with cap=0 returns 0 immediately (poll for emptiness).
 * Calling it back-to-back is safe -- consumed entries are removed
 * from the ring atomically. */
#define ABI_SYS_HOT_DRAIN       53

/*  --- Range 54..63: NEW in milestone 27A. Display introspection. */

/* SYS_DISPLAY_INFO: enumerate display outputs into a user buffer.
 *
 *   args:  (struct abi_display_info *out, uint32_t cap)
 *   ret:   number of records written (>= 0), or -ABI_E*.
 *
 * The kernel walks every known display output (M27G groundwork: only
 * one output is populated by the framebuffer/virtio-gpu paths today;
 * the loop here is the forward-compat shape). Each record carries
 * geometry, pixel format, backend identification, and live frame
 * counters so a userland tool can render a one-shot snapshot of the
 * display stack without extra syscalls. */
#define ABI_SYS_DISPLAY_INFO    54

/* M27C: per-window source-over alpha fill. Same packed (w,h) layout
 * as ABI_SYS_GUI_FILL, but the colour argument is interpreted as
 * 0xAARRGGBB and blended onto the window's existing pixels. */
#define ABI_SYS_GUI_FILL_ARGB   55

/* M27D: scaled / smoothed bitmap text into a window's backbuf.
 *   a1 = fd
 *   a2 = packed (x | y << 16) -- same as ABI_SYS_GUI_TEXT
 *   a3 = (const char *) string (kernel copies into a 256-byte buf)
 *   a4 = fg colour (0x00RRGGBB, GFX_TRANSPARENT for transparent bg
 *        is encoded into a5's high byte instead -- see below)
 *   a5 = packed (bg | scale<<24 | smooth<<31 reserved)
 *        layout chosen so an existing gui_text caller could trivially
 *        upgrade by switching the syscall number and packing scale=1.
 *
 * The kernel separates a5 into bg=a5&0x00FFFFFF, scale=(a5>>24)&0x7F,
 * smooth=(a5>>31)&1. A scale of 0 is treated as 1 for backwards
 * compatibility; values >32 are clamped (a 256-px-tall glyph is
 * already absurd). */
#define ABI_SYS_GUI_TEXT_SCALED 56

/* M27E: present-stats for the dirty-rect optimisation. Callers fill
 * an `abi_display_present_stats` struct and pass its address; the
 * kernel populates it with monotonic counters. Returns 0 on success,
 * -ABI_EINVAL if the destination pointer is NULL or unmapped. */
#define ABI_SYS_DISPLAY_PRESENT_STATS 57

struct abi_display_present_stats {
    uint64_t total_flips;
    uint64_t full_flips;
    uint64_t partial_flips;
    uint64_t empty_flips;
    uint64_t partial_pixels;
    uint64_t full_pixels;
    uint64_t cmp_full_frames;
    uint64_t cmp_partial_frames;
};
_Static_assert(sizeof(struct abi_display_present_stats) == 64,
               "abi_display_present_stats packed to 64 bytes");

/*  --- Range 58..79: NEW in milestone 28. Stability framework.  */

/* SYS_SLOG_READ: drain at most `cap` structured-log records from the
 * kernel ring into the caller's buffer.
 *
 *   args:  (struct abi_slog_record *out, uint32_t cap, uint64_t since_seq)
 *   ret:   number of records written (>= 0), or -ABI_E*.
 *
 * since_seq filters the ring -- only records with seq > since_seq are
 * returned. Pass 0 to fetch from the start of whatever the ring still
 * holds. The ring is bounded; on overwrite the oldest record is
 * silently discarded and a `dropped` counter on the very FIRST returned
 * record summarises how many entries were lost since the last drain.
 *
 * Records returned are deep copies; the kernel's ring entry is left
 * in place so a subsequent drain with a higher `since_seq` is safe.
 *
 * Records are returned in increasing seq order. Calling with cap == 0
 * returns 0 immediately ("peek for emptiness"). */
#define ABI_SYS_SLOG_READ      58

/* SYS_SLOG_WRITE: post a structured log line from userland into the
 * same kernel ring + console fan-out the kernel uses internally.
 *
 *   args:  (uint32_t level, const char *sub, const char *msg)
 *   ret:   0 on success, -ABI_E* on bad pointers or oversize.
 *
 * `level` is one of ABI_SLOG_LEVEL_*; `sub` is the subsystem tag
 * (NUL-terminated, capped at ABI_SLOG_SUB_MAX); `msg` is the line
 * itself (NUL-terminated, capped at ABI_SLOG_MSG_MAX). The kernel
 * stamps the timestamp + pid + sequence number. */
#define ABI_SYS_SLOG_WRITE     59

/* SYS_SLOG_STATS: snapshot per-level + per-subsystem counters into the
 * caller's buffer. Useful for logview --stats and the M28G stability
 * suite.
 *
 *   args:  (struct abi_slog_stats *out)
 *   ret:   0 on success, -ABI_EFAULT on bad pointer. */
#define ABI_SYS_SLOG_STATS     60

/* M28B crash dump enumerator. Returns the latest crash-dump file path
 * (if any) into the caller's buffer.
 *
 *   args:  (char *out, uint32_t cap, uint32_t *out_len)
 *   ret:   number of crash dumps available (>= 0). 0 means "no dumps".
 *          out is filled with the most recent dump's path (NUL-term),
 *          truncated to `cap` if necessary. out_len, if non-NULL,
 *          receives the dump's recorded size in bytes.
 */
#define ABI_SYS_CRASH_INFO     61

/* M28C watchdog query: how many ms have passed since the kernel
 * last petted the scheduler heartbeat. Useful for stabilitytest +
 * `wdog` shell builtin to confirm the watchdog is alive without
 * having to wait for it to bite. Returns ms since last heartbeat
 * (always >= 0). */
#define ABI_SYS_WDOG_STATUS    62

/* M28D safe-mode probe. Returns 1 if the kernel booted into safe
 * mode, 0 otherwise. Userland services consult this to decide whether
 * to come up at all. */
#define ABI_SYS_SAFE_MODE      63

/* M28E filesystem-check syscall. Run a structural integrity check
 * over the named mount path (e.g. "/", "/data", "/fat") and return
 * a small report.
 *
 *   args:  (const char *path, struct abi_fscheck_report *out)
 *   ret:   0 on success (report populated), -ABI_E* otherwise. */
#define ABI_SYS_FS_CHECK       64

/* M28F service-supervision query: enumerate registered services into
 * the caller's buffer, including restart counts and crash history.
 *
 *   args:  (struct abi_service_info *out, uint32_t cap)
 *   ret:   number of records written (>= 0). */
#define ABI_SYS_SVC_LIST       65

/* M28G stability self-test: run the kernel-resident stability sweep
 * and return a structured report. The userland `stabilitytest` tool
 * also runs userland-side checks and combines both. Returns 0 on
 * full PASS, -ABI_E* on FAIL; report is always populated. */
#define ABI_SYS_STAB_SELFTEST  66

/*  --- Range 67..79: NEW in milestone 29. Hardware bring-up framework. */

/* M29A SYS_HWINFO: snapshot the platform-wide hardware summary into
 * the caller's struct. CPU brand + feature flags, total/free memory
 * in pages, per-bus device counts, snapshot epoch + ms-uptime so
 * userspace can correlate with /data/hwinfo.snap.
 *
 *   args:  (struct abi_hwinfo_summary *out)
 *   ret:   0 on success, -ABI_EFAULT on a bad pointer.
 *
 * Cheap (no allocation, single pass over already-cached counters);
 * safe to call from any userland process. */
#define ABI_SYS_HWINFO         67

/* M29B SYS_DRVMATCH: report what driver (if any) the kernel bound to
 * a given PCI/USB device, and what fallback strategy was used.
 *
 *   args:  (uint32_t bus, uint32_t vendor, uint32_t device,
 *           struct abi_drvmatch_info *out)
 *   ret:   0 if a record was filled, -ABI_ENOENT if no matching
 *          device exists, -ABI_EFAULT on a bad pointer.
 *
 * The query key is bus|vendor|device (bus = ABI_DEVT_BUS_PCI or
 * ABI_DEVT_BUS_USB). For PCI the lookup walks the device table; for
 * USB it walks the xHCI introspection table. Result tells you which
 * driver name claimed the device, the match-table strategy used
 * (exact / class / generic / unsupported), and a one-line reason. */
#define ABI_SYS_DRVMATCH       68

/* M29C SYS_BOOT_DIAG: snapshot the per-boot diagnostics record (boot
 * mode, verbosity, devices-initialised counter, drivers-loaded /
 * -failed counter, services-started counter, time-to-prompt).
 *
 *   args:  (struct abi_boot_diag *out)
 *   ret:   0 on success, -ABI_EFAULT on a bad pointer.
 *
 * The kernel populates this struct as it boots and re-emits it from
 * the post-mortem path on every shutdown. /data/last_boot.diag is
 * the persistent on-disk twin written by the same code path. */
#define ABI_SYS_BOOT_DIAG      69

/* M31 SYS_NOTIFY_*: desktop-notification daemon entry points. The
 * kernel owns a fixed-size ring (NOTIFY_MAX) of recent notifications;
 * userspace can post into it, list snapshots out of it, and dismiss
 * individual records by id. The compositor pulls from the same ring
 * to paint toasts + the notification-center panel, so the same
 * pipeline serves kernel-emitted (boot, login, hotplug, service
 * crash) and user-emitted notifications. */
#define ABI_SYS_NOTIFY_POST    70  /* (const struct abi_notification *)
                                    * -> assigned id (>=1) or -ABI_E*  */
#define ABI_SYS_NOTIFY_LIST    71  /* (struct abi_notification *out,
                                    *  uint32_t cap)
                                    * -> count written or -ABI_E*      */
#define ABI_SYS_NOTIFY_DISMISS 72  /* (uint32_t id)
                                    *  id == 0 means "dismiss all"
                                    * -> 0 or -ABI_E*                  */

/* M35D SYS_HWCOMPAT_LIST: enumerate the runtime hardware-compatibility
 * database (PCI + USB devices joined with the static drvdb tier table
 * and live drvmatch outcomes).
 *
 *   args:  (struct abi_hwcompat_entry *out, uint32_t cap, uint32_t flags)
 *   ret:   number of entries written (>= 0), or -ABI_E*.
 *
 * `flags` is reserved (must be 0). The list is stable-ordered: PCI in
 * bus/slot/fn order first, then USB by attach order. The total can
 * exceed ABI_HWCOMPAT_MAX_ENTRIES; in that case the kernel returns the
 * first cap rows silently truncated -- callers can detect saturation
 * by comparing the return value against cap. The struct layout is
 * FROZEN; new fields go on the end behind a version field on the next
 * ABI bump. Cheap (no allocation, single pass over already-cached
 * tables); safe to call from any userland process. */
#define ABI_SYS_HWCOMPAT_LIST  73

/* Highest assigned syscall number plus one. */
#define ABI_SYS_NR_MAX          74

/* ============================================================
 *  Structured logging (Milestone 28A)
 * ============================================================ */

/* Severity levels. Numeric values match traditional syslog ordering
 * (lower = more severe) so a single `level <= ERROR` filter naturally
 * keeps the loud lines. */
#define ABI_SLOG_LEVEL_ERROR  0u
#define ABI_SLOG_LEVEL_WARN   1u
#define ABI_SLOG_LEVEL_INFO   2u
#define ABI_SLOG_LEVEL_DEBUG  3u
#define ABI_SLOG_LEVEL_MAX    4u

/* Subsystem tag length (incl. NUL). Tags are short, lowercase ASCII. */
#define ABI_SLOG_SUB_MAX      12

/* Per-record message body length (incl. NUL). Long lines are truncated
 * with a trailing "..." marker. */
#define ABI_SLOG_MSG_MAX      192

/* In-kernel ring depth. Must be a power of two for the wrap math.
 * 256 entries * 256 bytes = 64 KiB resident. */
#define ABI_SLOG_RING_DEPTH   256

/* One log record. Frozen layout (256 bytes total). Fields after `msg`
 * are reserved for forward-compat additions; new fields go in the
 * `_reserved` tail so the size stays pinned. */
struct abi_slog_record {
    uint64_t seq;                       /* monotonic per-boot id      */
    uint64_t time_ms;                   /* clock_ms() at emit         */
    uint32_t level;                     /* ABI_SLOG_LEVEL_*           */
    int32_t  pid;                       /* emitter pid (-1 = kernel) */
    char     sub [ABI_SLOG_SUB_MAX];    /* subsystem tag              */
    char     msg [ABI_SLOG_MSG_MAX];    /* line body                  */
    uint16_t dropped;                   /* lost since last drain (slot 0) */
    uint16_t flags;                     /* reserved, 0 in v1          */
    uint32_t _pad0;
    uint64_t _reserved[2];              /* sized so total == 256 B    */
};
_Static_assert(sizeof(struct abi_slog_record) == 256,
               "abi_slog_record layout is FROZEN at 256 bytes");

/* Aggregated counters returned by ABI_SYS_SLOG_STATS. */
struct abi_slog_stats {
    uint64_t total_emitted;             /* records written since boot */
    uint64_t total_dropped;             /* records overwritten in ring */
    uint64_t per_level[ABI_SLOG_LEVEL_MAX];
    uint64_t persist_bytes;             /* bytes flushed to disk      */
    uint64_t persist_flushes;           /* successful flush count     */
    uint64_t persist_failures;          /* failed flush attempts      */
    uint32_t ring_depth;
    uint32_t ring_in_use;               /* non-empty slots right now  */
};

/* ============================================================
 *  Crash dumps (Milestone 28B)
 * ============================================================ */

#define ABI_CRASH_PATH_MAX    96
#define ABI_CRASH_REASON_MAX  128

/* On-disk crash record header. The dump file (if writable) starts
 * with this, followed by the formatted text body up to `body_bytes`
 * long. Layout pinned for the userland `crashinfo` tool. */
struct abi_crash_header {
    uint32_t magic;                     /* 'CRSH' = 0x48535243        */
    uint32_t version;                   /* 1                          */
    uint64_t time_ms;                   /* boot time at panic (ms)    */
    uint64_t boot_seq;                  /* increments per boot         */
    uint32_t body_bytes;                /* trailing text body length   */
    int32_t  pid;                       /* current pid at panic       */
    char     reason[ABI_CRASH_REASON_MAX];
    uint64_t _reserved[4];
};
_Static_assert(sizeof(struct abi_crash_header) ==
               4 + 4 + 8 + 8 + 4 + 4 + ABI_CRASH_REASON_MAX + 32,
               "abi_crash_header layout is FROZEN");

/* ============================================================
 *  Watchdog status (Milestone 28C)
 * ============================================================ */

#define ABI_WDOG_REASON_MAX 96

/* Per-event reason kinds reported in last_reason / wdog log lines. */
#define ABI_WDOG_KIND_NONE           0u
#define ABI_WDOG_KIND_SCHED_STALL    1u  /* sched heartbeat stuck      */
#define ABI_WDOG_KIND_KERNEL_HANG    2u  /* PIT advanced but sched did not */
#define ABI_WDOG_KIND_USER_HANG      3u  /* userland process not yielding */
#define ABI_WDOG_KIND_MANUAL         4u  /* triggered by wdogtest --hang */

struct abi_wdog_status {
    uint32_t enabled;                    /* 0/1                        */
    uint32_t timeout_ms;                 /* configured threshold       */
    uint64_t kernel_heartbeats;          /* PIT IRQs observed          */
    uint64_t sched_heartbeats;           /* sched_yield/tick observed  */
    uint64_t syscall_heartbeats;         /* syscall_dispatch observed  */
    uint64_t ms_since_kernel_kick;       /* now - last_kernel_kick_ms  */
    uint64_t ms_since_sched_kick;        /* now - last_sched_kick_ms   */
    uint64_t event_count;                /* total events fired         */
    uint64_t last_event_ms;              /* time of most recent event  */
    uint32_t last_event_kind;            /* ABI_WDOG_KIND_*            */
    int32_t  last_event_pid;             /* offending pid (-1 if N/A)  */
    char     last_event_reason[ABI_WDOG_REASON_MAX];
    uint64_t _reserved[2];
};
_Static_assert(sizeof(struct abi_wdog_status) ==
               4 + 4 + 8 + 8 + 8 + 8 + 8 + 8 + 8 + 4 + 4 +
               ABI_WDOG_REASON_MAX + 16,
               "abi_wdog_status layout is FROZEN");

/* ============================================================
 *  Filesystem check (Milestone 28E)
 * ============================================================ */

#define ABI_FSCHECK_DETAIL_MAX  256
#define ABI_FSCHECK_PATH_MAX    64

/* Status flags reported by ABI_SYS_FS_CHECK. */
#define ABI_FSCHECK_OK            0x01u  /* mount is clean            */
#define ABI_FSCHECK_REPAIRED      0x02u  /* minor repair applied      */
#define ABI_FSCHECK_CORRUPT       0x04u  /* corruption detected       */
#define ABI_FSCHECK_UNMOUNTED     0x08u  /* path not mounted          */
#define ABI_FSCHECK_READ_ONLY     0x10u  /* mount is read-only        */

struct abi_fscheck_report {
    uint32_t status;                    /* OR of ABI_FSCHECK_* flags  */
    uint32_t errors_found;
    uint32_t errors_repaired;
    uint32_t reserved;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char     fs_type[16];               /* "tobyfs", "fat32", ...     */
    char     path[ABI_FSCHECK_PATH_MAX];
    char     detail[ABI_FSCHECK_DETAIL_MAX];
};

/* ============================================================
 *  Service supervision (Milestone 28F)
 * ============================================================ */

#define ABI_SVC_NAME_MAX  16
#define ABI_SVC_PATH_MAX  64

/* Service state -- mirrors enum service_state but ABI-stable. */
#define ABI_SVC_STATE_STOPPED    0u
#define ABI_SVC_STATE_RUNNING    1u
#define ABI_SVC_STATE_FAILED     2u
#define ABI_SVC_STATE_BACKOFF    3u    /* M28F: in restart cooldown  */
#define ABI_SVC_STATE_DISABLED   4u    /* M28F: tripped crash-loop   */

/* Service kind tag. Mirrors enum service_kind. */
#define ABI_SVC_KIND_BUILTIN     0u
#define ABI_SVC_KIND_PROGRAM     1u

/* Per-service record. Frozen layout (160 bytes). */
struct abi_service_info {
    char     name [ABI_SVC_NAME_MAX];
    char     path [ABI_SVC_PATH_MAX];
    uint32_t state;
    uint32_t kind;
    int32_t  pid;
    int32_t  last_exit;
    uint32_t restart_count;
    uint32_t crash_count;
    uint64_t last_start_ms;
    uint64_t last_crash_ms;
    uint64_t backoff_until_ms;
    uint8_t  autorestart;
    uint8_t  _pad[7];
    uint64_t _reserved[2];
};
_Static_assert(sizeof(struct abi_service_info) ==
               ABI_SVC_NAME_MAX + ABI_SVC_PATH_MAX +
               4 + 4 + 4 + 4 + 4 + 4 + 8 + 8 + 8 +
               1 + 7 + 16,
               "abi_service_info layout is FROZEN");

/* ============================================================
 *  Stability self-test (Milestone 28G)
 * ============================================================ */

#define ABI_STAB_DETAIL_MAX  256

/* Result mask returned by ABI_SYS_STAB_SELFTEST. Each bit set means
 * the named subsystem PASSED. Missing bits are FAILs/SKIPs that the
 * userland tool reports verbatim. */
#define ABI_STAB_OK_BOOT          0x0001u
#define ABI_STAB_OK_LOG           0x0002u
#define ABI_STAB_OK_PANIC         0x0004u
#define ABI_STAB_OK_WATCHDOG      0x0008u
#define ABI_STAB_OK_FILESYSTEM    0x0010u
#define ABI_STAB_OK_SERVICES      0x0020u
#define ABI_STAB_OK_GUI           0x0040u
#define ABI_STAB_OK_TERMINAL      0x0080u
#define ABI_STAB_OK_NETWORK       0x0100u
#define ABI_STAB_OK_INPUT         0x0200u
#define ABI_STAB_OK_SAFE_MODE     0x0400u
#define ABI_STAB_OK_DISPLAY       0x0800u
/* When all expected bits are set we have FULL PASS. */
#define ABI_STAB_OK_ALL           0x0FFFu

struct abi_stab_report {
    uint32_t result_mask;               /* OR of ABI_STAB_OK_*        */
    uint32_t expected_mask;             /* what we asked the kernel to test */
    uint32_t pass_count;
    uint32_t fail_count;
    uint64_t boot_ms;
    uint64_t safe_mode;                 /* 1 if booted safe          */
    char     detail[ABI_STAB_DETAIL_MAX];
};

/* ============================================================
 *  Peripheral test harness (Milestone 26A)
 * ============================================================ */

/* Bus-type tags for abi_dev_info::bus (and SYS_DEV_LIST type_mask). */
#define ABI_DEVT_BUS_PCI       0x01u
#define ABI_DEVT_BUS_USB       0x02u
#define ABI_DEVT_BUS_BLK       0x04u
#define ABI_DEVT_BUS_INPUT     0x08u
#define ABI_DEVT_BUS_AUDIO     0x10u
#define ABI_DEVT_BUS_BATTERY   0x20u
#define ABI_DEVT_BUS_HUB       0x40u
/* M27A: display outputs participate in the same dev-info enumeration
 * as every other peripheral so `devlist display` works without any
 * special-case syscall. */
#define ABI_DEVT_BUS_DISPLAY   0x80u
#define ABI_DEVT_BUS_ALL       0xFFu

/* Status flags for abi_dev_info::status. Bitfield. */
#define ABI_DEVT_PRESENT       0x01u   /* hardware detected this boot      */
#define ABI_DEVT_BOUND         0x02u   /* a kernel driver claimed it       */
#define ABI_DEVT_ACTIVE        0x04u   /* recently observed activity (IRQ) */
#define ABI_DEVT_ERROR         0x08u   /* driver flagged an error state    */
#define ABI_DEVT_REMOVED       0x10u   /* hot-plugged out, awaiting reap   */

/* Test-run "skip" value (positive so it's distinguishable from PASS=0
 * and the negative ABI_E* error space). Returned by SYS_DEV_TEST when
 * the underlying hardware is simply not present (e.g. battery test on
 * a desktop). */
#define ABI_DEVT_SKIP          1

/* Fixed-size strings inside abi_dev_info. The kernel always NUL-
 * terminates -- userland just iterates until '\0' or the cap. */
#define ABI_DEVT_NAME_MAX      32
#define ABI_DEVT_DRIVER_MAX    16
#define ABI_DEVT_EXTRA_MAX     64

/* Diagnostic-message buffer for SYS_DEV_TEST. */
#define ABI_DEVT_MSG_MAX       128

/* Maximum number of devices a single SYS_DEV_LIST call returns. The
 * userland tools allocate a buffer at this cap so they never have to
 * loop. Generous enough for any realistic single-board PC. */
#define ABI_DEVT_MAX_DEVICES   64

/* One device record. Frozen layout -- size is intentionally fixed at
 * 144 bytes so arrays of these can be passed across the syscall
 * boundary without versioning headaches. */
struct abi_dev_info {
    uint8_t  bus;             /* one of ABI_DEVT_BUS_*               */
    uint8_t  status;          /* OR of ABI_DEVT_* status flags       */
    uint8_t  hub_depth;       /* 0 = root, 1 = behind first hub, ... */
    uint8_t  hub_port;        /* downstream port number (1..N)       */
    uint16_t vendor;          /* PCI vid OR USB idVendor (0 if N/A)  */
    uint16_t device;          /* PCI did OR USB idProduct            */
    uint8_t  class_code;      /* PCI base class OR USB iface class   */
    uint8_t  subclass;        /* PCI subclass     OR USB iface sub   */
    uint8_t  prog_if;         /* PCI prog-if      OR USB iface proto */
    uint8_t  index;           /* per-bus index (PCI dev #, blk slot, ...) */
    uint32_t _pad0;
    char     name  [ABI_DEVT_NAME_MAX];     /* "xhci0", "ata0", "usb1-3"   */
    char     driver[ABI_DEVT_DRIVER_MAX];   /* bound driver name or ""     */
    char     extra [ABI_DEVT_EXTRA_MAX];    /* free-form ("4096 sectors")  */
};
_Static_assert(sizeof(struct abi_dev_info) == 16 + ABI_DEVT_NAME_MAX +
               ABI_DEVT_DRIVER_MAX + ABI_DEVT_EXTRA_MAX,
               "abi_dev_info layout is FROZEN");

/* ============================================================
 *  Hot-plug events (Milestone 26C)
 * ============================================================ */

/* Action codes for abi_hot_event::action. */
#define ABI_HOT_ATTACH      1u   /* device became present                  */
#define ABI_HOT_DETACH      2u   /* device went away (cable yanked, etc)   */
#define ABI_HOT_ERROR       3u   /* driver flagged a bound device unhappy  */

/* Bus tags reuse ABI_DEVT_BUS_* (USB / HUB are the only ones populated
 * in M26C; PCI/audio/battery hot-plug land later). */

/* Per-event diagnostic blurb cap (incl trailing NUL). */
#define ABI_HOT_INFO_MAX   48

/* In-kernel ring depth. Userland can consume any number per call; the
 * cap only matters for overflow accounting. */
#define ABI_DEVT_HOT_RING  64

/* One hot-plug record. Frozen layout: 64 bytes, no padding, so arrays
 * cross the syscall boundary cleanly. */
struct abi_hot_event {
    uint64_t seq;       /* monotonically incrementing per-boot id      */
    uint64_t time_ms;   /* clock_ms() at emit time                      */
    uint8_t  bus;       /* one of ABI_DEVT_BUS_*                        */
    uint8_t  action;    /* one of ABI_HOT_*                             */
    uint8_t  hub_depth; /* 0 = root, 1 = behind hub, ...               */
    uint8_t  hub_port;  /* hub-port number for behind-hub events        */
    uint16_t slot;      /* USB slot id / PCI bdf packed into low 16     */
    uint16_t dropped;   /* events lost since last drain (set on idx 0) */
    char     info[ABI_HOT_INFO_MAX]; /* short driver-supplied blurb    */
};
_Static_assert(sizeof(struct abi_hot_event) == 16 + 8 + ABI_HOT_INFO_MAX,
               "abi_hot_event layout is FROZEN");

struct abi_dlmap_info {
    uint64_t base;        /* echo of caller's requested base */
    uint64_t entry;       /* e_entry + base                   */
    uint64_t dynamic;     /* VA of PT_DYNAMIC contents (0 = absent) */
    uint64_t phdr;        /* VA of program-header table       */
    uint16_t phnum;
    uint16_t phent;
    uint32_t _pad;
};

/* ============================================================
 *  Display introspection (Milestone 27A)
 * ============================================================ */

/* Pixel format codes for abi_display_info::pixel_format.
 *
 * The kernel's gfx layer always exposes a 32-bit BGRX little-endian
 * surface to userland regardless of how the underlying scanout is
 * wired (Limine FB, virtio-gpu host resource). New formats grow
 * additively; old userland binaries that only know XRGB8888 still
 * work because the back-buffer they read from gfx_backbuf() stays
 * in that one canonical layout.
 *
 * Numbers follow the "fourcc-style" packed convention to make a
 * future move to DRM_FORMAT_* trivial. */
#define ABI_DISPLAY_FMT_UNKNOWN  0
#define ABI_DISPLAY_FMT_XRGB8888 1     /* 0x00RRGGBB in a uint32_t   */
#define ABI_DISPLAY_FMT_ARGB8888 2     /* 0xAARRGGBB; M27C alpha     */

/* Display backend tags. Identifies which gfx_backend drove the most
 * recent gfx_flip(). Userland tools render this verbatim so the
 * regression suites can grep for a specific transport. */
#define ABI_DISPLAY_BACKEND_NONE       0
#define ABI_DISPLAY_BACKEND_LIMINE_FB  1
#define ABI_DISPLAY_BACKEND_VIRTIO_GPU 2

/* Status flags for abi_display_info::status. Same shape as the
 * ABI_DEVT_* status mask so existing userland renderers can be reused. */
#define ABI_DISPLAY_PRESENT     0x01u   /* output detected by gfx_init() */
#define ABI_DISPLAY_PRIMARY     0x02u   /* desktop is targeting this one */
#define ABI_DISPLAY_ACTIVE      0x04u   /* compositor is flipping to it  */

#define ABI_DISPLAY_NAME_MAX    32
#define ABI_DISPLAY_BACKEND_MAX 16
#define ABI_DISPLAY_MAX_OUTPUTS 4

/* One display-output record. 96-byte layout (extended in M27G to add
 * origin_x/origin_y for multi-monitor groundwork; pre-M27G consumers
 * that hard-coded sizeof() == 88 must be recompiled).
 *
 * origin_x/origin_y are signed because secondary monitors can sit to
 * the LEFT of the primary in the layout space (negative origin_x);
 * arrangements set up later via display_set_origin() carry through.
 * On a single-monitor system the primary is always (0, 0). */
struct abi_display_info {
    uint8_t  index;              /* 0 = primary, 1.. = secondaries     */
    uint8_t  status;             /* OR of ABI_DISPLAY_*                */
    uint8_t  pixel_format;       /* one of ABI_DISPLAY_FMT_*           */
    uint8_t  backend_id;         /* one of ABI_DISPLAY_BACKEND_*       */
    uint32_t width;              /* logical pixel width                */
    uint32_t height;             /* logical pixel height               */
    uint32_t pitch_bytes;        /* row stride in bytes (>= width*bpp) */
    uint32_t bpp;                /* bits per pixel (typically 32)      */
    int32_t  origin_x;           /* M27G: layout origin (px, signed)   */
    int32_t  origin_y;           /* M27G: layout origin (px, signed)   */
    uint32_t _pad0;              /* explicit alignment for next u64    */
    uint64_t flips;              /* total gfx_flip() count to this o/p */
    uint64_t last_flip_ns;       /* perf_now_ns() of most recent flip  */
    char     name   [ABI_DISPLAY_NAME_MAX];     /* "fb0", "virtio-gpu0" */
    char     backend[ABI_DISPLAY_BACKEND_MAX];  /* "limine-fb", ...    */
};
/* Layout breakdown (96 bytes on the typical 32-byte name/16-byte
 * backend ABI defaults):
 *   4   index/status/pixel_format/backend_id (4 x u8)
 *   16  width/height/pitch_bytes/bpp        (4 x u32)
 *   12  origin_x/origin_y/_pad0             (3 x u32)
 *   16  flips/last_flip_ns                  (2 x u64)
 *   32  name                                (NAME_MAX)
 *   16  backend                             (BACKEND_MAX)
 *   --
 *   96 */
_Static_assert(sizeof(struct abi_display_info) ==
                   4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 8 + 8 +
                   ABI_DISPLAY_NAME_MAX + ABI_DISPLAY_BACKEND_MAX,
               "abi_display_info layout extended for M27G multi-monitor");

/* ============================================================
 *  open() flags (for ABI_SYS_OPEN)
 * ============================================================
 *
 * Bitfield. ACCMODE in low 3 bits. */
#define ABI_O_RDONLY    0x0
#define ABI_O_WRONLY    0x1
#define ABI_O_RDWR      0x2
#define ABI_O_ACCMODE   0x3

#define ABI_O_CREAT     0x040
#define ABI_O_EXCL      0x080
#define ABI_O_TRUNC     0x200
#define ABI_O_APPEND    0x400

/* ============================================================
 *  lseek() whence values
 * ============================================================ */
#define ABI_SEEK_SET    0
#define ABI_SEEK_CUR    1
#define ABI_SEEK_END    2

/* ============================================================
 *  waitpid flags
 * ============================================================ */
#define ABI_WNOHANG     0x1   /* don't block; return 0 if no child ready */

/* ============================================================
 *  stat structure
 * ============================================================ */

/* File type bits encoded in abi_stat::mode (top half). */
#define ABI_S_IFMT      0xF000
#define ABI_S_IFREG     0x8000
#define ABI_S_IFDIR     0x4000

/* Permission bits (low 9 bits of mode). */
#define ABI_S_IRUSR     0400
#define ABI_S_IWUSR     0200
#define ABI_S_IXUSR     0100
#define ABI_S_IRGRP     0040
#define ABI_S_IWGRP     0020
#define ABI_S_IXGRP     0010
#define ABI_S_IROTH     0004
#define ABI_S_IWOTH     0002
#define ABI_S_IXOTH     0001

#define ABI_S_ISDIR(m)  (((m) & ABI_S_IFMT) == ABI_S_IFDIR)
#define ABI_S_ISREG(m)  (((m) & ABI_S_IFMT) == ABI_S_IFREG)

struct abi_stat {
    uint64_t size;
    uint32_t mode;       /* ABI_S_IFMT bits + permission bits */
    uint32_t uid;
    uint32_t gid;
    uint32_t _reserved0; /* padding for forward ABI growth */
    uint64_t _reserved1[2];
};

/* ============================================================
 *  spawn()
 * ============================================================
 *
 * abi_spawn_req packs everything sys_spawn needs into a single
 * pointer arg so the syscall stays inside the 6-register ABI.
 *
 * `path`           : NUL-terminated absolute path to the ELF.
 * `argv`           : NULL-terminated array of NUL-terminated strings.
 *                    May be NULL -> spawn with argc=0.
 * `envp`           : NULL-terminated array of "KEY=VALUE" strings.
 *                    May be NULL -> child inherits parent's env.
 * `fd0/fd1/fd2`    : file descriptors to inherit as child's stdin/
 *                    stdout/stderr. -1 = "use parent's same fd"
 *                    (after dup); -2 = "console default". The
 *                    explicit fd is duped into the child's fd table
 *                    by the kernel, so the parent can close its own
 *                    copy independently after spawn returns.
 * `flags`          : reserved, must be 0 in v1.
 *
 * Returns: child pid (>0) on success; negative ABI_E* on failure.
 * Process is launched synchronously — when this call returns the
 * child is enqueued and ready to run. Use SYS_WAITPID to block on
 * the child's exit.
 */
struct abi_spawn_req {
    const char         *path;
    char *const        *argv;       /* NULL-terminated */
    char *const        *envp;       /* NULL-terminated; NULL = inherit */
    int                 fd0;
    int                 fd1;
    int                 fd2;
    uint32_t            flags;
};

/* fd0/fd1/fd2 sentinels for spawn_req. */
#define ABI_SPAWN_FD_INHERIT   (-1)
#define ABI_SPAWN_FD_CONSOLE   (-2)

/* ============================================================
 *  Networking helpers (already exported in older syscall.h, but
 *  the canonical layout lives here now).
 * ============================================================ */

#define ABI_AF_INET        2
#define ABI_SOCK_DGRAM     2

struct abi_sockaddr_in_be {
    uint32_t ip;            /* network byte order */
    uint16_t port;          /* network byte order */
    uint16_t _pad;
};

/* ============================================================
 *  VFS dirent (used by SYS_FS_READDIR -- frozen layout).
 * ============================================================ */

#define ABI_DT_FILE   1
#define ABI_DT_DIR    2

#define ABI_DIRENT_NAME_MAX  64

struct abi_dirent {
    char     name[ABI_DIRENT_NAME_MAX];
    uint32_t type;          /* ABI_DT_FILE | ABI_DT_DIR */
    uint32_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
};

/* ============================================================
 *  Auxiliary vector (Milestone 25D).
 * ============================================================
 *
 * On process startup the kernel pushes onto the user stack:
 *
 *   argc
 *   argv[0..argc-1], NULL
 *   envp[0..envc-1], NULL
 *   auxv[0..n-1], { AT_NULL, 0 }      <-- NEW in 25D
 *   <string pool>
 *
 * Each auxv entry is two 64-bit words: { uint64_t a_type, uint64_t
 * a_val }. Layout matches SysV / Linux exactly so we can grow toward
 * compatibility without renumbering. The dynamic linker (ld-toby.so)
 * reads this vector to discover where the program's own PHDRs live
 * (AT_PHDR/AT_PHNUM/AT_PHENT), where it itself was loaded
 * (AT_BASE), and where to hand off control after relocations
 * (AT_ENTRY).
 *
 * Static ET_EXEC programs that ignore auxv are unaffected -- they
 * just walk past it via the trailing AT_NULL.
 */
#define ABI_AT_NULL     0
#define ABI_AT_IGNORE   1
#define ABI_AT_EXECFD   2
#define ABI_AT_PHDR     3       /* address of program headers in memory */
#define ABI_AT_PHENT    4       /* size of one program header */
#define ABI_AT_PHNUM    5       /* number of program headers */
#define ABI_AT_PAGESZ   6       /* system page size in bytes */
#define ABI_AT_BASE     7       /* base address the interpreter was loaded at */
#define ABI_AT_FLAGS    8       /* flags */
#define ABI_AT_ENTRY    9       /* entry point of the *program* (not interp) */

struct abi_auxv {
    uint64_t a_type;
    uint64_t a_val;
};

/* ============================================================
 *  Hardware bring-up framework (Milestone 29)
 * ============================================================ */

/* M29A: machine-wide hardware summary. CPU brand + feature flags,
 * memory totals (in 4K pages), per-bus device counts, snapshot
 * epoch + ms-uptime. Frozen 256-byte layout so userland can mmap or
 * write it to disk without versioning headaches. */

#define ABI_HW_CPU_VENDOR_MAX  16   /* incl. NUL                 */
#define ABI_HW_CPU_BRAND_MAX   48   /* CPUID 0x80000002..4 brand */
#define ABI_HW_PROFILE_MAX     16   /* "vm", "desktop", "laptop" */

/* CPU feature bits returned in abi_hwinfo_summary::cpu_features.
 * These mirror the standard CPUID bits the kernel cares about; new
 * bits go on the end. */
#define ABI_HW_CPU_FEAT_FPU      0x00000001u  /* CPUID.01H:EDX[0]   */
#define ABI_HW_CPU_FEAT_TSC      0x00000010u  /* CPUID.01H:EDX[4]   */
#define ABI_HW_CPU_FEAT_MSR      0x00000020u  /* CPUID.01H:EDX[5]   */
#define ABI_HW_CPU_FEAT_PAE      0x00000040u  /* CPUID.01H:EDX[6]   */
#define ABI_HW_CPU_FEAT_APIC     0x00000200u  /* CPUID.01H:EDX[9]   */
#define ABI_HW_CPU_FEAT_SSE      0x02000000u  /* CPUID.01H:EDX[25]  */
#define ABI_HW_CPU_FEAT_SSE2     0x04000000u  /* CPUID.01H:EDX[26]  */
#define ABI_HW_CPU_FEAT_HT       0x10000000u  /* CPUID.01H:EDX[28]  */
#define ABI_HW_CPU_FEAT_LM       0x20000000u  /* long mode (synthesized) */
#define ABI_HW_CPU_FEAT_HYPER    0x40000000u  /* CPUID.01H:ECX[31] hypervisor */
#define ABI_HW_CPU_FEAT_NX       0x80000000u  /* CPUID.80000001H:EDX[20] */

struct abi_hwinfo_summary {
    /* --- CPU --- */
    char     cpu_vendor[ABI_HW_CPU_VENDOR_MAX]; /* "GenuineIntel"  */
    char     cpu_brand [ABI_HW_CPU_BRAND_MAX];  /* "QEMU Virtual CPU..." */
    uint32_t cpu_count;        /* logical CPUs detected (smp_total)  */
    uint32_t cpu_family;       /* CPUID.01H Family ID                */
    uint32_t cpu_model;        /* CPUID.01H Model ID                 */
    uint32_t cpu_stepping;     /* CPUID.01H Stepping ID              */
    uint32_t cpu_features;     /* OR of ABI_HW_CPU_FEAT_*            */
    uint32_t _pad0;            /* explicit pad so next u64 is aligned */

    /* --- Memory (in 4 KiB pages so values fit comfortably in u32) --- */
    uint64_t mem_total_pages;
    uint64_t mem_used_pages;
    uint64_t mem_free_pages;

    /* --- Per-bus device counts (snapshot of devtest_enumerate). --- */
    uint16_t pci_count;
    uint16_t usb_count;
    uint16_t blk_count;
    uint16_t input_count;
    uint16_t audio_count;
    uint16_t battery_count;
    uint16_t hub_count;
    uint16_t display_count;

    /* --- Snapshot bookkeeping. --- */
    uint64_t boot_uptime_ms;   /* clock_ms at snapshot               */
    uint64_t snapshot_epoch;   /* monotonic counter, bumped per snap */
    uint32_t kernel_abi_ver;   /* TOBY_ABI_VERSION                   */
    uint32_t safe_mode;        /* 1 if booted in safe mode           */
    char     profile_hint[ABI_HW_PROFILE_MAX]; /* "vm"|"desktop"|"laptop" */

    uint64_t _reserved[3];
};
_Static_assert(sizeof(struct abi_hwinfo_summary) == 192,
               "abi_hwinfo_summary layout is FROZEN at 192 bytes");

/* ============================================================
 *  M29B: driver match + fallback record
 * ============================================================ */

#define ABI_DRVMATCH_DRIVER_MAX 16
#define ABI_DRVMATCH_REASON_MAX 64

/* Strategy by which the kernel picked a driver for the device.
 * Values are stable; new strategies go on the end. */
#define ABI_DRVMATCH_NONE        0u  /* no match found at all     */
#define ABI_DRVMATCH_EXACT       1u  /* vendor:device pair matched */
#define ABI_DRVMATCH_CLASS       2u  /* class/subclass match       */
#define ABI_DRVMATCH_GENERIC     3u  /* fallback generic driver    */
#define ABI_DRVMATCH_UNSUPPORTED 4u  /* known unsupported, degraded */
#define ABI_DRVMATCH_FORCED_OFF  5u  /* manually disabled by user  */

struct abi_drvmatch_info {
    uint32_t bus;       /* one of ABI_DEVT_BUS_* (PCI / USB)        */
    uint32_t vendor;
    uint32_t device;
    uint32_t class_code;
    uint32_t subclass;
    uint32_t prog_if;
    uint32_t strategy;  /* one of ABI_DRVMATCH_*                    */
    uint32_t bound;     /* 1 if a driver currently owns the device  */
    char     driver[ABI_DRVMATCH_DRIVER_MAX];
    char     reason[ABI_DRVMATCH_REASON_MAX]; /* short explanation */
};

/* ============================================================
 *  M29C: per-boot diagnostics record
 * ============================================================ */

#define ABI_BOOT_TAG_MAX 16

/* Boot-mode codes (mirror safemode levels but with explicit values). */
#define ABI_BOOT_MODE_NORMAL         0u
#define ABI_BOOT_MODE_SAFE_BASIC     1u  /* M29C: minimal drivers only   */
#define ABI_BOOT_MODE_SAFE_GUI       2u  /* M29C: + framebuffer + input  */
#define ABI_BOOT_MODE_VERBOSE        3u  /* normal + verbose-trace prints */
#define ABI_BOOT_MODE_COMPATIBILITY  4u  /* M35E: GUI+net but reduced
                                          * drivers (no audio, no
                                          * non-HID USB, no virtio-gpu)
                                          * for hardware that gets weird
                                          * with the full driver set    */

struct abi_boot_diag {
    uint32_t mode;             /* one of ABI_BOOT_MODE_*               */
    uint32_t verbose;          /* 1 if verbose flag was set            */
    uint32_t devices_initialised;
    uint32_t drivers_loaded;
    uint32_t drivers_failed;
    uint32_t services_started;
    uint32_t services_failed;
    uint32_t boot_seq;         /* increments per boot                  */
    uint64_t time_to_prompt_ms;
    uint64_t boot_start_ms;
    uint64_t snapshot_ms;
    char     last_failed_driver[ABI_DRVMATCH_DRIVER_MAX];
    char     boot_tag[ABI_BOOT_TAG_MAX];
    uint64_t _reserved[2];
};
_Static_assert(sizeof(struct abi_boot_diag) ==
               4*8 + 8 + 8 + 8 + ABI_DRVMATCH_DRIVER_MAX +
               ABI_BOOT_TAG_MAX + 16,
               "abi_boot_diag layout is FROZEN");

/* ============================================================
 *  Desktop notifications (Milestone 31)
 * ============================================================ */

/* Urgency levels. Drives the toast accent colour + the notification-
 * center icon. Higher = louder. INFO is the default. */
#define ABI_NOTIFY_URG_INFO   0u
#define ABI_NOTIFY_URG_WARN   1u
#define ABI_NOTIFY_URG_ERR    2u

/* Source-of-record tags. Purely informational -- the compositor uses
 * them to choose an icon hint, but treats unknown values as GENERIC. */
#define ABI_NOTIFY_KIND_GENERIC  0u
#define ABI_NOTIFY_KIND_SYSTEM   1u   /* kernel boot / shutdown / safe */
#define ABI_NOTIFY_KIND_DEVICE   2u   /* hotplug add/remove            */
#define ABI_NOTIFY_KIND_SERVICE  3u   /* service supervisor events     */
#define ABI_NOTIFY_KIND_USER     4u   /* userspace SYS_NOTIFY_POST     */
#define ABI_NOTIFY_KIND_NET      5u   /* networking events             */

/* `flags` bits inside an abi_notification record. */
#define ABI_NOTIFY_FLAG_DISMISSED  (1u << 0)  /* user dismissed       */
#define ABI_NOTIFY_FLAG_DISPLAYED  (1u << 1)  /* shown as toast once  */

/* Field caps. Sized so the whole record is 200 bytes -- comfortable
 * for a 32-entry ring (6.4 KiB resident) and roomy enough for
 * "Service 'login' restarted (3rd time)" without truncation. */
#define ABI_NOTIFY_APP_MAX     16
#define ABI_NOTIFY_TITLE_MAX   48
#define ABI_NOTIFY_BODY_MAX    96

/* On-the-wire notification record. Frozen layout (200 bytes).
 *   id        -- monotonic per-boot, never zero for a real entry.
 *                Userspace passes 0 in for a notify_post; the kernel
 *                fills it in on success.
 *   kind      -- ABI_NOTIFY_KIND_*
 *   urgency   -- ABI_NOTIFY_URG_*
 *   flags     -- ABI_NOTIFY_FLAG_* mask
 *   time_ms   -- post timestamp (boot-relative ms)
 *   app       -- short emitter tag, e.g. "kernel", "session"
 *   title     -- one-line headline (drawn bold-ish in the toast)
 *   body      -- one-paragraph detail, optional ('' = title-only)
 *   _reserved -- forward-compat slack; userspace MUST zero it.        */
struct abi_notification {
    uint32_t id;
    uint32_t kind;
    uint32_t urgency;
    uint32_t flags;
    uint64_t time_ms;
    char     app  [ABI_NOTIFY_APP_MAX];
    char     title[ABI_NOTIFY_TITLE_MAX];
    char     body [ABI_NOTIFY_BODY_MAX];
    uint64_t _reserved[2];
};
_Static_assert(sizeof(struct abi_notification) ==
               4*4 + 8 + ABI_NOTIFY_APP_MAX + ABI_NOTIFY_TITLE_MAX +
               ABI_NOTIFY_BODY_MAX + 16,
               "abi_notification layout is FROZEN at 200 bytes");

/* ============================================================
 *  M35D: hardware compatibility database
 * ============================================================
 *
 * One row per attached PCI/USB device. Combines:
 *   - the live bus-enumeration result (vendor, product, class triple)
 *   - the static drvdb knowledge base (friendly chip name + tier)
 *   - the live drvmatch outcome (which driver bound, or why nothing did)
 *
 * Status mirrors the DRVDB_* tier exposed to userland so the same
 * vocabulary survives end-to-end:
 *
 *   ABI_HWCOMPAT_SUPPORTED   drvdb says the in-tree driver should
 *                            work AND a driver is currently bound;
 *                            the device is fully usable.
 *   ABI_HWCOMPAT_PARTIAL     a driver is bound but only covers a
 *                            subset of features (e.g. virtio-net w/o
 *                            multiqueue), OR drvdb says SUPPORTED
 *                            but no driver bound this boot (e.g.
 *                            blacklisted via /etc/drvmatch.conf).
 *   ABI_HWCOMPAT_UNSUPPORTED drvdb explicitly tags the device as
 *                            unsupported, OR no driver claimed the
 *                            device and drvdb has no record of it.
 *   ABI_HWCOMPAT_UNKNOWN     reserved (kernel never emits this; the
 *                            value is exposed so userland tools can
 *                            initialise zeroed buffers and tell them
 *                            apart from real status codes).
 *
 * The `bound` byte is independent of `status`: a forced-off driver
 * row reports status=PARTIAL bound=0; a fully-supported HID keyboard
 * reports status=SUPPORTED bound=1. */

#define ABI_HWCOMPAT_UNKNOWN       0u
#define ABI_HWCOMPAT_SUPPORTED     1u
#define ABI_HWCOMPAT_PARTIAL       2u
#define ABI_HWCOMPAT_UNSUPPORTED   3u

#define ABI_HWCOMPAT_FRIENDLY_MAX  48
#define ABI_HWCOMPAT_DRIVER_MAX    16
#define ABI_HWCOMPAT_REASON_MAX    64

/* Hard cap on records returned by a single SYS_HWCOMPAT_LIST call.
 * Generous enough for any QEMU configuration we ship (~10 PCI + a
 * handful of USB) while keeping the userland staging buffer well
 * under 64 KiB (64 * 144 = 9 KiB). */
#define ABI_HWCOMPAT_MAX_ENTRIES   64

/* Frozen 144-byte record (matches abi_dev_info's footprint). New
 * fields go in the trailing `_reserved` slot on the next ABI bump. */
struct abi_hwcompat_entry {
    uint8_t  bus;        /* one of ABI_DEVT_BUS_* (PCI / USB)        */
    uint8_t  status;     /* one of ABI_HWCOMPAT_*                    */
    uint8_t  bound;      /* 1 if a kernel driver currently owns it   */
    uint8_t  _pad0;
    uint16_t vendor;     /* PCI vid OR USB idVendor (0 if unknown)   */
    uint16_t product;    /* PCI did OR USB idProduct                 */
    uint8_t  class_code; /* PCI base class OR USB iface/dev class    */
    uint8_t  subclass;   /* PCI subclass    OR USB subclass          */
    uint8_t  prog_if;    /* PCI prog-if     OR USB protocol          */
    uint8_t  _pad1;
    uint32_t _pad2;      /* keeps header to 16 bytes for alignment   */
    char     friendly[ABI_HWCOMPAT_FRIENDLY_MAX]; /* drvdb-derived   */
    char     driver  [ABI_HWCOMPAT_DRIVER_MAX];   /* "" if unbound   */
    char     reason  [ABI_HWCOMPAT_REASON_MAX];   /* short blurb     */
};
_Static_assert(sizeof(struct abi_hwcompat_entry) ==
               16 + ABI_HWCOMPAT_FRIENDLY_MAX +
               ABI_HWCOMPAT_DRIVER_MAX + ABI_HWCOMPAT_REASON_MAX,
               "abi_hwcompat_entry layout is FROZEN at 144 bytes");

#ifdef __cplusplus
}
#endif

#endif /* TOBYOS_ABI_ABI_H */
