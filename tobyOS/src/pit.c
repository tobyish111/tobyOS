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
    if (g_hz == 0) return;
    /* ticks_to_wait = ms * hz / 1000; round up so very small sleeps
     * still wait at least one tick. */
    uint64_t wait = (ms * (uint64_t)g_hz + 999) / 1000;
    uint64_t end  = pit_ticks() + wait;
    while (pit_ticks() < end) {
        hlt();
    }
}
