/* programs/rendertest/main.c -- M27A: per-case rendering validator.
 *
 * Each "case" exercises one rendering invariant and reports PASS, SKIP
 * or FAIL on its own line so a regression script can grep the exact
 * scenario it cares about. Cases:
 *
 *   basic       displayinfo round-trip + dimensions sanity
 *   geometry    pitch >= width*bpp/8, 32-bpp surface
 *   primitives  fill + text + clip via a transient window (no leak)
 *   overlap     two windows stacked, both flip without crashing
 *   cursor      after a window cycle the mouse cursor is still drawn
 *               (verified indirectly: gfx_flip count keeps incrementing)
 *   alpha       SKIP until M27C
 *   font        SKIP until M27D (large/anti-aliased font path)
 *   dirty       SKIP until M27E (dirty-rectangle tracker)
 *
 * The default invocation runs every case. Pass case names on the
 * command line to filter:  rendertest basic primitives
 *
 * Exit codes:
 *   0   no FAILs (skips OK).
 *   1   at least one FAIL.
 *   2   bad usage. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include <tobyos_devtest.h>

/* ---- raw GUI syscalls (mirror drawtest -- no libtoby wrappers) ---- */

#define SYS_YIELD            5
#define SYS_GUI_CREATE      10
#define SYS_GUI_FILL        11
#define SYS_GUI_TEXT        12
#define SYS_GUI_FLIP        13
#define SYS_GUI_POLL_EVENT  14
#define SYS_CLOSE            4
#define SYS_GUI_FILL_ARGB   55  /* M27C */

#define GUI_TRANSPARENT_BG   0xFF000000u

struct gui_event {
    int           type;
    int           x;
    int           y;
    unsigned char button;
    unsigned char key;
    unsigned char _pad[2];
};

static inline void sys_yield(void) {
    /* rax is BOTH input (syscall number) AND output (return value),
     * so it must be tied with "0"(...). Otherwise the compiler thinks
     * rax still holds SYS_YIELD across consecutive sys_yield() calls,
     * elides the reload, and the second `syscall` fires with whatever
     * the kernel left in rax (0 = ABI_SYS_EXIT) -- killing the proc. */
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_YIELD)
        : "rcx", "r11", "memory");
    (void)r;
}
static inline int sys_close(int fd) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_CLOSE), "D"((long)fd)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_create(unsigned w, unsigned h, const char *title) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_CREATE), "D"((long)w), "S"((long)h), "d"(title)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_fill(int fd, int x, int y, int w, int h,
                               unsigned color) {
    long r;
    unsigned whlen = ((unsigned)(unsigned short)w) |
                     (((unsigned)(unsigned short)h) << 16);
    register long r10 __asm__("r10") = (long)whlen;
    register long r8  __asm__("r8")  = (long)color;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FILL), "D"((long)fd),
          "S"((long)x), "d"((long)y),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_text(int fd, int x, int y, const char *s,
                               unsigned fg, unsigned bg) {
    long r;
    unsigned xy = ((unsigned)(unsigned short)x) |
                  (((unsigned)(unsigned short)y) << 16);
    register long r10 __asm__("r10") = (long)fg;
    register long r8  __asm__("r8")  = (long)bg;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_TEXT), "D"((long)fd),
          "S"((long)xy), "d"(s),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_flip(int fd) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FLIP), "D"((long)fd)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_fill_argb(int fd, int x, int y, int w, int h,
                                    unsigned argb) {
    long r;
    unsigned whlen = ((unsigned)(unsigned short)w) |
                     (((unsigned)(unsigned short)h) << 16);
    register long r10 __asm__("r10") = (long)whlen;
    register long r8  __asm__("r8")  = (long)argb;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FILL_ARGB), "D"((long)fd),
          "S"((long)x), "d"((long)y),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}

/* M27E -- the kernel exposes a stats snapshot via SYS_DISPLAY_PRESENT_STATS
 * (=57). We don't have a libtoby wrapper yet, so do the syscall by hand.
 * The struct lifetime ends at function exit, so the kernel doesn't have
 * to keep the pointer alive across calls. */
#define SYS_DISPLAY_PRESENT_STATS 57

struct rt_stats {
    unsigned long long total_flips;
    unsigned long long full_flips;
    unsigned long long partial_flips;
    unsigned long long empty_flips;
    unsigned long long partial_pixels;
    unsigned long long full_pixels;
    unsigned long long cmp_full_frames;
    unsigned long long cmp_partial_frames;
};

static int rt_present_stats(struct rt_stats *s) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_DISPLAY_PRESENT_STATS), "D"((long)s)
        : "rcx", "r11", "memory");
    return (int)r;
}

/* ---- result aggregation ----------------------------------------- */

static int g_pass, g_fail, g_skip;

static void emit(const char *case_name, int rc, const char *msg) {
    const char *tag;
    if      (rc == 0)              { tag = "PASS"; g_pass++; }
    else if (rc == ABI_DEVT_SKIP)  { tag = "SKIP"; g_skip++; }
    else                           { tag = "FAIL"; g_fail++; }
    printf("[%s] rendertest %-10s -- %s\n", tag, case_name,
           msg && msg[0] ? msg : "(no message)");
}

/* ---- a case is a (name, fn) pair --------------------------------- */

typedef int (*case_fn)(char *msg, size_t cap);

struct rcase {
    const char *name;
    case_fn     fn;
};

/* ---- helper: snapshot the primary display ----------------------- */

static int primary_snapshot(struct abi_display_info *out) {
    static struct abi_display_info recs[ABI_DISPLAY_MAX_OUTPUTS];
    int n = tobydisp_list(recs, ABI_DISPLAY_MAX_OUTPUTS);
    if (n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        if (recs[i].status & ABI_DISPLAY_PRIMARY) {
            *out = recs[i];
            return 0;
        }
    }
    /* No flag set -- treat slot 0 as primary fallback. */
    *out = recs[0];
    return 0;
}

/* ---- cases ------------------------------------------------------ */

static int case_basic(char *msg, size_t cap) {
    struct abi_display_info p;
    if (primary_snapshot(&p) < 0) {
        snprintf(msg, cap, "no primary display present");
        return ABI_DEVT_SKIP;
    }
    if (p.width == 0 || p.height == 0) {
        snprintf(msg, cap, "primary has zero geometry %ux%u",
                 (unsigned)p.width, (unsigned)p.height);
        return -1;
    }
    snprintf(msg, cap, "%s %ux%u backend=%s flips=%lu",
             p.name[0] ? p.name : "(unnamed)",
             (unsigned)p.width, (unsigned)p.height,
             p.backend[0] ? p.backend : "(none)",
             (unsigned long)p.flips);
    return 0;
}

static int case_geometry(char *msg, size_t cap) {
    struct abi_display_info p;
    if (primary_snapshot(&p) < 0) {
        snprintf(msg, cap, "no primary display present");
        return ABI_DEVT_SKIP;
    }
    if (p.bpp != 32) {
        snprintf(msg, cap, "expected 32-bpp surface, got %u", (unsigned)p.bpp);
        return -1;
    }
    unsigned min_pitch = p.width * (p.bpp / 8u);
    if (p.pitch_bytes < min_pitch) {
        snprintf(msg, cap, "pitch %u < minimum %u",
                 (unsigned)p.pitch_bytes, min_pitch);
        return -1;
    }
    snprintf(msg, cap, "bpp=%u pitch=%u (>=min %u)",
             (unsigned)p.bpp, (unsigned)p.pitch_bytes, min_pitch);
    return 0;
}

static int case_primitives(char *msg, size_t cap) {
    struct abi_display_info p;
    if (primary_snapshot(&p) < 0) {
        snprintf(msg, cap, "no primary display");
        return ABI_DEVT_SKIP;
    }
    int fd = sys_gui_create(160, 100, "rt-prim");
    if (fd < 0) {
        snprintf(msg, cap, "sys_gui_create failed rc=%d", fd);
        return -1;
    }
    int errs = 0;
    if (sys_gui_fill(fd, 0, 0, 160, 100, 0x00224466u) < 0) errs++;
    if (sys_gui_fill(fd, 8, 8,  64,  32, 0x00FF8800u) < 0) errs++;
    if (sys_gui_fill(fd, -10, 50, 40, 30, 0x00FF00FFu) < 0) errs++;  /* clip */
    if (sys_gui_text(fd, 8, 80, "rt-prim",
                     0x00FFFFFFu, GUI_TRANSPARENT_BG) < 0) errs++;
    if (sys_gui_flip(fd) < 0) errs++;
    sys_close(fd);

    if (errs) {
        snprintf(msg, cap, "%d primitive(s) returned <0", errs);
        return -1;
    }
    snprintf(msg, cap, "5 primitives + flip + close OK");
    return 0;
}

static int case_overlap(char *msg, size_t cap) {
    struct abi_display_info p;
    if (primary_snapshot(&p) < 0) {
        snprintf(msg, cap, "no primary display");
        return ABI_DEVT_SKIP;
    }
    int fd1 = sys_gui_create(120, 80, "rt-back");
    if (fd1 < 0) {
        snprintf(msg, cap, "create #1 failed rc=%d", fd1);
        return -1;
    }
    int fd2 = sys_gui_create(120, 80, "rt-front");
    if (fd2 < 0) {
        snprintf(msg, cap, "create #2 failed rc=%d", fd2);
        sys_close(fd1);
        return -1;
    }
    /* Two coloured windows stacked. The compositor picks the z-order;
     * this just verifies the second create + flip didn't hose the first. */
    int errs = 0;
    errs += (sys_gui_fill(fd1, 0, 0, 120, 80, 0x00553311u) < 0);
    errs += (sys_gui_fill(fd2, 0, 0, 120, 80, 0x00115533u) < 0);
    errs += (sys_gui_flip(fd1) < 0);
    errs += (sys_gui_flip(fd2) < 0);
    sys_close(fd2);
    sys_close(fd1);

    if (errs) {
        snprintf(msg, cap, "overlap pass produced %d errors", errs);
        return -1;
    }
    snprintf(msg, cap, "two stacked 120x80 windows flipped + closed");
    return 0;
}

static int case_cursor(char *msg, size_t cap) {
    /* The cursor is composited unconditionally on every gfx_flip().
     * We can't introspect the actual cursor pixels from userland,
     * but we CAN assert the flip counter increments after a window
     * create/destroy cycle -- if the compositor were stuck the count
     * would be frozen and the cursor would visually leave a trail.
     * Indirect, but adequate for a regression check.
     *
     * Caveat: the M27A boot harness runs BEFORE the kernel idle loop
     * starts driving the compositor. In that window flips==0 and the
     * test cannot prove anything; we SKIP cleanly so the bootscript
     * doesn't FAIL. The same case re-runs interactively from the
     * shell where flips will already be non-zero and the assertion
     * has teeth. */
    struct abi_display_info before, after;
    if (primary_snapshot(&before) < 0) {
        snprintf(msg, cap, "no primary display");
        return ABI_DEVT_SKIP;
    }
    /* Boot-harness skip. Pre-M27E the only signal was display.flips==0,
     * but M27E's kernel-side display_test_dirty selftest now drives
     * gfx_flip() three times before any userland tool runs, so flips
     * is already non-zero at boot. Use the COMPOSITOR frame counters
     * (cmp_full + cmp_partial) as the real "did pid 0 ever tick the
     * compositor?" signal -- those only advance from gui_tick on
     * pid 0, which the boot harness blocks. */
    struct rt_stats st;
    if (rt_present_stats(&st) >= 0 &&
        (st.cmp_full_frames + st.cmp_partial_frames) == 0) {
        snprintf(msg, cap,
                 "compositor has not produced any frames yet "
                 "(boot harness, cmp=0) -- skipping cursor advance check");
        return ABI_DEVT_SKIP;
    }
    if (before.flips == 0) {
        snprintf(msg, cap,
                 "compositor has not produced any frames yet "
                 "(boot harness, flips=0) -- skipping cursor advance check");
        return ABI_DEVT_SKIP;
    }
    int fd = sys_gui_create(80, 60, "rt-cursor");
    if (fd < 0) {
        snprintf(msg, cap, "create failed rc=%d", fd);
        return -1;
    }
    sys_gui_fill(fd, 0, 0, 80, 60, 0x00808080u);
    sys_gui_flip(fd);
    sys_yield();
    sys_yield();
    sys_close(fd);
    /* A couple more yields so the compositor repaints the region the
     * window vacated. There's no "force-compositor-pass" syscall. */
    sys_yield();
    sys_yield();
    if (primary_snapshot(&after) < 0) {
        snprintf(msg, cap, "displayinfo dropped between snapshots");
        return -1;
    }
    if (after.flips <= before.flips) {
        snprintf(msg, cap,
                 "flip counter did not advance (%lu -> %lu) -- "
                 "compositor stuck?",
                 (unsigned long)before.flips,
                 (unsigned long)after.flips);
        return -1;
    }
    snprintf(msg, cap, "flips advanced %lu -> %lu after window cycle",
             (unsigned long)before.flips,
             (unsigned long)after.flips);
    return 0;
}

static int case_backend(char *msg, size_t cap) {
    /* M27B: verify the backend metadata is consistent. The kernel
     * stamps the backend name into both the `backend` string and the
     * `backend_id` byte; if those disagree the userland-side enum
     * lookup would surface a mismatched label and we want to catch
     * that here. We also assert the name is one of the known set --
     * a third-party backend would render as id=0 (NONE) which would
     * make a desktop install very confusing. */
    struct abi_display_info p;
    if (primary_snapshot(&p) < 0) {
        snprintf(msg, cap, "no primary display");
        return ABI_DEVT_SKIP;
    }
    if (!p.backend[0]) {
        snprintf(msg, cap, "primary backend name is empty");
        return -1;
    }
    const char *id_str = tobydisp_backend_str(p.backend_id);
    /* tobydisp_backend_str returns "(none)" for unknown IDs. The
     * registered string MUST match a known ID so id->name mappings
     * stay consistent across drvtest/devlist/displayinfo. */
    if (strcmp(id_str, "(none)") == 0 &&
        p.backend_id != ABI_DISPLAY_BACKEND_NONE) {
        snprintf(msg, cap,
                 "backend_id %u is unknown (registered name=%s)",
                 (unsigned)p.backend_id, p.backend);
        return -1;
    }
    if (p.backend_id == ABI_DISPLAY_BACKEND_NONE) {
        snprintf(msg, cap,
                 "backend has no recognised id (raw name=%s)", p.backend);
        return -1;
    }
    snprintf(msg, cap,
             "backend=%s id=%u (mapped=%s) bpp=%u pitch=%u",
             p.backend, (unsigned)p.backend_id, id_str,
             (unsigned)p.bpp, (unsigned)p.pitch_bytes);
    return 0;
}

static int case_alpha(char *msg, size_t cap) {
    /* M27C: paint a stack of overlapping translucent rectangles into
     * a scratch window and confirm every blend call succeeds. The
     * actual math is verified by the kernel-side display_test_alpha
     * (asserted on the same boot via devtest_boot_run); here we just
     * make sure the syscall plumbing + clipping behave for both
     * translucent overlays and the alpha=0 / alpha=255 edge cases.
     * Any out-of-bounds blend would emit a kernel log line that the
     * test_m27c.ps1 forbidden-token list catches. */
    int fd = sys_gui_create(320, 220, "rt-alpha");
    if (fd < 0) {
        snprintf(msg, cap, "gui_create failed (%d)", fd);
        return -1;
    }
    int rc = 0;
    rc |= sys_gui_fill     (fd,   0,   0, 320, 220, 0x00202020u);
    rc |= sys_gui_fill_argb(fd,  20,  20, 180, 100, 0x80FF4040u);
    rc |= sys_gui_fill_argb(fd, 100,  60, 180, 100, 0x8040FF40u);
    rc |= sys_gui_fill_argb(fd, 180, 100, 120,  90, 0x804040FFu);
    /* alpha=0 -> kernel must short-circuit (no writes, no error). */
    rc |= sys_gui_fill_argb(fd,   0,   0,  10,  10, 0x00FF00FFu);
    /* alpha=255 -> kernel must take the opaque-fast path. */
    rc |= sys_gui_fill_argb(fd, 300, 200,  20,  20, 0xFF00AA00u);
    /* Off-window negative coords -> clip to nothing, must return 0. */
    int clip_rc = sys_gui_fill_argb(fd, -50, -50, 30, 30, 0x80808080u);
    if (clip_rc < 0) rc = -1;
    rc |= sys_gui_flip(fd);
    sys_close(fd);
    if (rc != 0) {
        snprintf(msg, cap,
                 "one or more blend calls returned non-zero (rc=%d)", rc);
        return -1;
    }
    snprintf(msg, cap,
             "6 ARGB overlays committed (3 translucent + 3 edge cases)");
    return 0;
}

static int case_font(char *msg, size_t cap) {
    snprintf(msg, cap, "anti-aliased / large fonts arrive in M27D");
    return ABI_DEVT_SKIP;
}

static int case_dirty(char *msg, size_t cap) {
    /* M27E userland validation.
     *
     * The boot harness model blocks pid 0 in proc_wait while we run,
     * so the compositor (gated on pid 0 from gui_tick) cannot tick
     * during this test. The kernel-side display_test_dirty drives
     * gfx_flip() directly during devtest_boot_run() *before* this
     * test runs, so by the time we sample stats the per-flip
     * counters are guaranteed non-zero.
     *
     * What we validate here:
     *   1. SYS_DISPLAY_PRESENT_STATS plumbing (two snapshot calls).
     *   2. Counter invariants:
     *        total_flips == full_flips + partial_flips + empty_flips
     *        partial_flips >= 1   (kernel-side test took that path)
     *   3. The window we open + flip doesn't crash and the syscalls
     *      return 0 on success.
     *
     * We deliberately do NOT require the COMPOSITOR's per-frame
     * counters to advance during this call -- they can only move
     * when pid 0 runs, which it cannot under boot harness. The
     * kernel-side validation already confirmed the gfx_flip
     * stat-bookkeeping branches work. */

    /* (1) Initial snapshot. */
    struct rt_stats before;
    if (rt_present_stats(&before) < 0) {
        snprintf(msg, cap, "first present_stats syscall failed");
        return -1;
    }

    /* Counter consistency check on the BEFORE snapshot. */
    unsigned long sum_before = (unsigned long)(before.full_flips +
                                               before.partial_flips +
                                               before.empty_flips);
    if (sum_before != (unsigned long)before.total_flips) {
        snprintf(msg, cap,
                 "stat invariant broken: total=%lu but full+partial+empty=%lu",
                 (unsigned long)before.total_flips, sum_before);
        return -1;
    }

    /* M27E is fundamentally broken if no partial flip ever happened.
     * The kernel-side display_test_dirty self-test drives one
     * partial gfx_flip during boot devtest, so this should always
     * be >= 1 by the time userland runs. */
    if (before.partial_flips < 1) {
        snprintf(msg, cap,
                 "no partial flips on record (total=%lu full=%lu "
                 "partial=%lu empty=%lu) -- gfx_flip's partial path "
                 "is broken or the kernel selftest didn't run",
                 (unsigned long)before.total_flips,
                 (unsigned long)before.full_flips,
                 (unsigned long)before.partial_flips,
                 (unsigned long)before.empty_flips);
        return -1;
    }

    /* (2) Exercise the GUI write path -- create a window, draw, flip.
     * These all succeed without crashing the kernel even though the
     * compositor can't actually paint them in this thread context. */
    int fd = sys_gui_create(160, 100, "rt-dirty");
    if (fd < 0) {
        snprintf(msg, cap, "gui_create failed (rc=%d)", fd);
        return -1;
    }
    int errs = 0;
    for (int i = 0; i < 4; i++) {
        errs += (sys_gui_fill(fd, 0, 0, 160, 100,
                              0x00100000u | ((unsigned)i * 0x40u)) < 0);
        errs += (sys_gui_flip(fd) < 0);
    }
    sys_close(fd);
    if (errs != 0) {
        snprintf(msg, cap, "fill/flip path returned %d errors", errs);
        return -1;
    }

    /* (3) Final snapshot + invariant check. */
    struct rt_stats after;
    if (rt_present_stats(&after) < 0) {
        snprintf(msg, cap, "second present_stats syscall failed");
        return -1;
    }
    unsigned long sum_after = (unsigned long)(after.full_flips +
                                              after.partial_flips +
                                              after.empty_flips);
    if (sum_after != (unsigned long)after.total_flips) {
        snprintf(msg, cap,
                 "stat invariant broken AFTER: total=%lu but sum=%lu",
                 (unsigned long)after.total_flips, sum_after);
        return -1;
    }

    /* Stats are monotonic -- AFTER must be >= BEFORE on every counter. */
    if (after.total_flips        < before.total_flips        ||
        after.full_flips         < before.full_flips         ||
        after.partial_flips      < before.partial_flips      ||
        after.empty_flips        < before.empty_flips        ||
        after.cmp_full_frames    < before.cmp_full_frames    ||
        after.cmp_partial_frames < before.cmp_partial_frames) {
        snprintf(msg, cap, "stats went backwards across snapshots");
        return -1;
    }

    /* Final summary -- the regex in test_m27e.ps1 looks for this
     * exact format. Every field is the delta across our window
     * activity (which may all be 0 in boot harness mode -- that's
     * fine, the kernel test already validated the actual feature). */
    unsigned long flip_d  = (unsigned long)(after.total_flips   - before.total_flips);
    unsigned long full_d  = (unsigned long)(after.full_flips    - before.full_flips);
    unsigned long part_d  = (unsigned long)(after.partial_flips - before.partial_flips);
    unsigned long cmp_d   = (unsigned long)
        ((after.cmp_full_frames + after.cmp_partial_frames) -
         (before.cmp_full_frames + before.cmp_partial_frames));
    snprintf(msg, cap,
             "flips +%lu (full+%lu partial+%lu); compositor frames +%lu "
             "(baseline: total=%lu partial=%lu)",
             flip_d, full_d, part_d, cmp_d,
             (unsigned long)before.total_flips,
             (unsigned long)before.partial_flips);
    return 0;
}

/* ---- driver ---------------------------------------------------- */

static const struct rcase g_cases[] = {
    { "basic",      case_basic      },
    { "geometry",   case_geometry   },
    { "primitives", case_primitives },
    { "overlap",    case_overlap    },
    { "cursor",     case_cursor     },
    { "backend",    case_backend    },
    { "alpha",      case_alpha      },
    { "font",       case_font       },
    { "dirty",      case_dirty      },
};
#define N_CASES ((int)(sizeof(g_cases) / sizeof(g_cases[0])))

static const struct rcase *find_case(const char *name) {
    for (int i = 0; i < N_CASES; i++) {
        if (!strcmp(g_cases[i].name, name)) return &g_cases[i];
    }
    return 0;
}

static void run_case(const struct rcase *rc) {
    char msg[ABI_DEVT_MSG_MAX];
    msg[0] = '\0';
    int v = rc->fn(msg, sizeof msg);
    emit(rc->name, v, msg);
}

int main(int argc, char **argv) {
    if (argc <= 1) {
        for (int i = 0; i < N_CASES; i++) run_case(&g_cases[i]);
    } else {
        for (int i = 1; i < argc; i++) {
            const struct rcase *rc = find_case(argv[i]);
            if (!rc) {
                fprintf(stderr, "FAIL: rendertest: unknown case '%s'\n",
                        argv[i]);
                fprintf(stderr, "available: ");
                for (int j = 0; j < N_CASES; j++) {
                    fprintf(stderr, "%s%s", g_cases[j].name,
                            j + 1 < N_CASES ? " " : "\n");
                }
                return 2;
            }
            run_case(rc);
        }
    }
    int total = g_pass + g_fail + g_skip;
    const char *tag = (g_fail == 0) ? "PASS" : "FAIL";
    printf("%s: rendertest: pass=%d skip=%d fail=%d (total=%d)\n",
           tag, g_pass, g_skip, g_fail, total);
    return g_fail ? 1 : 0;
}
