/* src/stacktrace.c -- Kernel stack unwinding (Phase 8).
 *
 * Walks the RBP chain to capture return addresses. Optionally resolves
 * addresses to symbol names if a symbol table is registered.
 */

#include <tobyos/stacktrace.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* Simple symbol table for address resolution */
#define MAX_SYMBOLS 512

struct sym_entry {
    uint64_t addr;
    const char *name;
};

static struct sym_entry g_symbols[MAX_SYMBOLS];
static int g_nsymbols = 0;

void stacktrace_init(void) {
    g_nsymbols = 0;
    kprintf("[stacktrace] unwinder ready\n");
}

void stacktrace_add_symbol(uint64_t addr, const char *name) {
    if (g_nsymbols >= MAX_SYMBOLS) return;
    g_symbols[g_nsymbols].addr = addr;
    g_symbols[g_nsymbols].name = name;
    g_nsymbols++;
}

static const char *resolve_symbol(uint64_t addr) {
    const char *best = 0;
    uint64_t best_dist = (uint64_t)-1;

    for (int i = 0; i < g_nsymbols; i++) {
        if (g_symbols[i].addr <= addr) {
            uint64_t dist = addr - g_symbols[i].addr;
            if (dist < best_dist) {
                best_dist = dist;
                best = g_symbols[i].name;
            }
        }
    }

    /* Only return if reasonably close (within 64K -- likely same function) */
    if (best && best_dist < 0x10000) return best;
    return 0;
}

static int is_kernel_addr(uint64_t addr) {
    return (addr >= 0xFFFF800000000000ULL);
}

int stacktrace_capture(struct stack_frame *frames, int max_frames) {
    if (!frames || max_frames <= 0) return 0;

    uint64_t rbp;
    __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));

    int count = 0;
    while (rbp && count < max_frames) {
        if (!is_kernel_addr(rbp)) break;
        /* Verify alignment */
        if (rbp & 7) break;

        uint64_t *frame = (uint64_t *)rbp;
        uint64_t saved_rbp = frame[0];
        uint64_t ret_addr  = frame[1];

        if (!ret_addr) break;

        frames[count].rbp = rbp;
        frames[count].rip = ret_addr;
        frames[count].symbol = resolve_symbol(ret_addr);
        count++;

        if (saved_rbp <= rbp) break;  /* Prevent infinite loops */
        rbp = saved_rbp;
    }
    return count;
}

void stacktrace_print(void) {
    struct stack_frame frames[STACKTRACE_MAX_FRAMES];
    int n = stacktrace_capture(frames, STACKTRACE_MAX_FRAMES);

    kprintf("--- stack trace (%d frames) ---\n", n);
    for (int i = 0; i < n; i++) {
        if (frames[i].symbol) {
            kprintf("  #%d  0x%016lx  %s\n", i, frames[i].rip, frames[i].symbol);
        } else {
            kprintf("  #%d  0x%016lx  <unknown>\n", i, frames[i].rip);
        }
    }
    kprintf("--- end trace ---\n");
}

void stacktrace_print_from(uint64_t rbp, uint64_t rip) {
    kprintf("--- stack trace from RIP=0x%016lx RBP=0x%016lx ---\n", rip, rbp);

    /* Print the initial frame */
    const char *sym = resolve_symbol(rip);
    if (sym) {
        kprintf("  #0  0x%016lx  %s\n", rip, sym);
    } else {
        kprintf("  #0  0x%016lx  <unknown>\n", rip);
    }

    int count = 1;
    while (rbp && count < STACKTRACE_MAX_FRAMES) {
        if (!is_kernel_addr(rbp)) break;
        if (rbp & 7) break;

        uint64_t *frame = (uint64_t *)rbp;
        uint64_t saved_rbp = frame[0];
        uint64_t ret_addr  = frame[1];

        if (!ret_addr) break;

        sym = resolve_symbol(ret_addr);
        if (sym) {
            kprintf("  #%d  0x%016lx  %s\n", count, ret_addr, sym);
        } else {
            kprintf("  #%d  0x%016lx  <unknown>\n", count, ret_addr);
        }
        count++;

        if (saved_rbp <= rbp) break;
        rbp = saved_rbp;
    }
    kprintf("--- end trace ---\n");
}
