/* pit.c -- 8254 PIT channel-0 driver wired to IRQ0.
 *
 * Mode-3 (square wave) is what the PC traditionally uses for the timer
 * tick: the chip toggles its OUT pin every (divisor/2) base-clock
 * cycles, so IRQ0 fires at base_freq / divisor Hz.
 *
 * The IRQ handler does the bare minimum -- bump a counter, EOI the
 * master PIC -- so it stays cheap and re-entrancy-free. Anything else
 * (logging, scheduler ticks, etc.) belongs in the main loop, which
 * polls pit_ticks() between hlts.
 */

#include <tobyos/pit.h>
#include <tobyos/irq.h>
#include <tobyos/isr.h>
#include <tobyos/cpu.h>
#include <tobyos/signal.h>
#include <tobyos/printk.h>
#include <tobyos/watchdog.h>
#include <tobyos/sched.h>
#include <tobyos/proc.h>
#include <tobyos/xhci.h>
#include <tobyos/perf.h>

#define PIT_CH0_DATA  0x40
#define PIT_CMD       0x43

/* Mode-3 (square wave) on channel 0, low-byte then high-byte access. */
#define PIT_CMD_CH0_LOHI_MODE3  0x36

static volatile uint64_t g_ticks = 0;
static uint32_t          g_hz    = 0;

static void pit_irq(struct regs *r) {
    g_ticks++;
    irq_eoi_isa(0);

    /* M28C: feed the watchdog. wdog_kick_kernel() is a single store;
     * wdog_check() throttles itself to ~1 Hz internally and only
     * touches its own globals + slog (both IRQ-safe). */
    wdog_kick_kernel();
    wdog_check();

    /* Poll USB HID every PIT tick. On real hardware where xHCI MSI/MSI-X
     * may not fire, this guarantees USB keyboards and mice get sampled at
     * the PIT rate regardless of what's running. xhci_poll() is a fast
     * no-op when nothing is pending (just reads the event ring dequeue). */
    xhci_poll();

    /* Asynchronous-signal preemption point.
     *
     * If the timer interrupted ring 3 (CPL=3) and the current process
     * has any pending signal, kill it right now -- this is what makes
     * a CPU-bound user loop killable by Ctrl+C even though the shell
     * never gets a chance to call sched_yield itself.
     *
     * proc_exit() never returns; it sched_yields to whatever's next
     * READY. The IRQ trap-frame on this kstack is harmless: the kstack
     * gets freed when the parent reaps. EOI was already sent above so
     * future PIT IRQs can fire on the new running proc. */
    if ((r->cs & 3) == 3) {
        signal_deliver_if_pending();

        /* Timer preemption: if we interrupted a user-mode process that
         * is NOT pid 0, force a yield so the kernel idle loop (pid 0)
         * can run the compositor and service input. This bounds worst-
         * case input-to-pixel latency to one PIT period (4ms @ 250Hz).
         *
         * Safety: the user trap frame is already saved on this kstack.
         * sched_yield() → proc_context_switch() saves the ISR call
         * chain; when we're rescheduled later, we resume here and
         * return through iretq normally. Same mechanism used by
         * signal_deliver_if_pending → proc_exit → sched_yield above. */
        struct proc *cur = current_proc();
        if (cur && cur->pid != 0) {
            sched_yield();
        }
    }
}

void pit_init(uint32_t hz) {
    if (hz == 0) hz = 100;

    uint32_t divisor = PIT_BASE_FREQ_HZ / hz;
    if (divisor == 0)        divisor = 1;
    else if (divisor > 0xFFFF) divisor = 0xFFFF;

    g_hz = PIT_BASE_FREQ_HZ / divisor;

    outb(PIT_CMD,      PIT_CMD_CH0_LOHI_MODE3);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    /* Register + unmask through the facade -- in PIC mode this is just
     * isr_register + pic_unmask(0); after irq_switch_to_ioapic() it
     * becomes isr_register + ioapic_route(GSI_for_IRQ0). Either way
     * the same IDT vector (0x20) is used. */
    irq_install_isa(0, pit_irq);

    kprintf("[pit] channel 0 -> %u Hz (divisor=%u)\n",
            (unsigned)g_hz, (unsigned)divisor);
}

uint64_t pit_ticks(void) { return g_ticks; }
uint32_t pit_hz(void)    { return g_hz; }

void pit_sleep_ms(uint64_t ms) {
    /* Robust busy delay. Two reasons this no longer does `while (pit_ticks() <
     * end) hlt();`:
     *
     *  1. It must not depend on the PIT IRQ still advancing. The scheduler
     *     moves the tick to the LAPIC timer early in boot, and on headless
     *     QEMU/TCG (and defensively on real HW) the IOAPIC->LAPIC PIT edge can
     *     stop being delivered. A pit_ticks()-based wait would then spin/sleep
     *     forever.
     *  2. It must not park the core in HLT. Once every core is halted, a
     *     dropped timer wake never returns from HLT and the box freezes with no
     *     fault (the classic "stall ~3s into boot"). PAUSE keeps the core live
     *     so the deadline is always re-checked and any pending IRQ is taken at
     *     an instruction boundary.
     *
     * Prefer the TSC monotonic clock (perf_now_ns), which keeps advancing no
     * matter what happens to interrupt delivery. */
    uint64_t now_ns = perf_now_ns();
    if (now_ns != 0) {
        uint64_t end_ns = now_ns + ms * 1000000ull;
        while (perf_now_ns() < end_ns)
            __asm__ volatile ("pause" ::: "memory");
        return;
    }

    /* Pre-TSC fallback (very early boot, before perf_init): the PIT IRQ is
     * still the only clock we have. Spin on PAUSE rather than HLT so a missed
     * wake can't wedge us; this window is short and single-threaded. */
    if (g_hz == 0) return;
    uint64_t wait = (ms * (uint64_t)g_hz + 999) / 1000;
    uint64_t end  = pit_ticks() + wait;
    while (pit_ticks() < end)
        __asm__ volatile ("pause" ::: "memory");
}
