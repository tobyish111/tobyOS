/* serial.c -- COM1 driver + QEMU debugcon mux.
 *
 * Every byte goes to BOTH 0xE9 (debugcon, no init required) and COM1.
 * COM1 is brought up lazily on the first serial_putc(); serial_init()
 * is idempotent. Capture with QEMU `-serial` and `-debugcon`.
 *
 * TX is NON-BLOCKING once armed (serial_enable_tx_irq). The old driver
 * busy-waited on the THR-empty bit for EVERY byte, which is free in QEMU
 * (its THRE is always set) but ~260 us/byte on real hardware at 38400
 * baud -- a ~150-byte log line stalled the CPU ~40 ms. Because kprintf
 * runs under g_printk_lock with local IRQs disabled, and the desktop
 * traces from the mouse IRQ, that turned the whole GUI to molasses on
 * real HW (see the gui-unify-tobytk bring-up). Now:
 *
 *   - serial_putc() pushes into a ring and returns (never spins).
 *   - the UART's THRE interrupt (IRQ4) drains the ring 16 bytes at a
 *     time as the FIFO empties.
 *   - serial_tx_pump() (called from the pid-0 idle loop) ALSO drains the
 *     ring, so output flows even if IRQ4 never routes on a given board
 *     (the PCI serial here reports IRQ10 -- legacy IRQ4 is not a sure
 *     thing). The THRE IRQ is therefore a latency optimisation, not a
 *     correctness dependency.
 *   - before the IRQ is armed (early boot) and during a panic, we fall
 *     back to a BOUNDED spin so a wedged UART can never hang the CPU.
 */

#include <tobyos/serial.h>
#include <tobyos/cpu.h>
#include <tobyos/spinlock.h>
#include <tobyos/isr.h>
#include <tobyos/irq.h>

#define COM1            0x3F8
#define COM1_DATA       (COM1 + 0)
#define COM1_INT_EN     (COM1 + 1)   /* IER */
#define COM1_IIR        (COM1 + 2)   /* read: interrupt id; write: FIFO ctrl */
#define COM1_FIFO_CTRL  (COM1 + 2)
#define COM1_LINE_CTRL  (COM1 + 3)
#define COM1_MODEM_CTRL (COM1 + 4)
#define COM1_LINE_STAT  (COM1 + 5)   /* LSR */

#define LINE_STAT_THR_EMPTY 0x20     /* LSR bit5: transmit FIFO empty */
#define IER_ETBEI           0x02     /* IER bit1: THR-empty interrupt enable */
#define UART_FIFO_DEPTH     16       /* 16550 TX FIFO: room for 16 when THRE=1 */

#define DBGCON_PORT 0xE9

/* COM1 IRQ4 -> IDT vector (IRQ_ISA_VECTOR(4) = 0x24); wired post-IOAPIC. */
#define COM1_ISA_IRQ 4

/* Bounded-spin cap for the sync path. Normally THRE clears in well under
 * a thousand iterations; the cap only bites if the UART is wedged, and
 * keeps a wedged port from hanging the box rather than dropping a byte. */
#define SERIAL_SPIN_MAX 2000000u

/* Baud configuration. The 16550 input clock is 115200 Hz, so the divisor
 * latch value is 115200 / baud. We default to 38400 (divisor 3) because
 * that is what the documented real-hardware capture setup uses
 * (docs/smep-ap-capture.md -- HP EliteDesk null-modem @ 38400 8N1). To
 * monitor at the more common 115200, set COM1_BAUD to 115200 (divisor 1)
 * and configure the capture terminal to match. */
#define UART_INPUT_CLOCK 115200u
#define COM1_BAUD        38400u
#define COM1_DIVISOR     (UART_INPUT_CLOCK / COM1_BAUD)

static bool s_serial_ready = false;

/* TX ring. Power-of-two size so head/tail wrap with a mask. Guarded by
 * g_tx_lock (irqsave): serial_putc enqueues, the THRE IRQ + pump dequeue.
 * Sized so a burst of trace lines fits without dropping under normal use. */
/* 8 KiB was too small once a workload keeps every CPU busy. The ring drains
 * from the THRE IRQ and from a safety-net drain in pid 0's IDLE loop -- and
 * chrome keeps ~20 threads runnable, so pid 0 effectively never idles. Output
 * then outruns the drain, the ring fills, and bytes are dropped SILENTLY. That
 * reads exactly like the machine froze: the log (heartbeat included) simply
 * stops mid-line while the system is still running. It also explains why the
 * apparent "hang" moved EARLIER as more tracing was added -- more bytes fill
 * the ring sooner. */
#define TXR_SIZE 262144u
#define TXR_MASK (TXR_SIZE - 1u)
static char              g_txr[TXR_SIZE];
static volatile uint32_t g_txr_head;   /* producer */
static volatile uint32_t g_txr_tail;   /* consumer */
static volatile uint64_t g_txr_dropped;/* bytes lost to a full ring */
static spinlock_t        g_tx_lock = SPINLOCK_INIT;

/* How many log bytes have been silently discarded. Non-zero means the serial
 * log is INCOMPLETE and any "it stopped here" conclusion drawn from it is
 * unsafe. */
uint64_t serial_dropped(void) { return g_txr_dropped; }

/* false = bounded-spin sync mode (default, early boot, panic);
 * true  = async ring mode (after serial_enable_tx_irq). */
static bool s_tx_irq_mode = false;

unsigned serial_baud(void) { return COM1_BAUD; }

static inline void dbgcon_putc(char c) {
    outb(DBGCON_PORT, (uint8_t)c);
}

static inline bool txr_empty(void) { return g_txr_head == g_txr_tail; }
static inline bool txr_full(void)  { return ((g_txr_head + 1u) & TXR_MASK) == g_txr_tail; }

void serial_init(void) {
    if (s_serial_ready) return;
    outb(COM1_INT_EN,     0x00);                       /* mask all UART interrupts */
    outb(COM1_LINE_CTRL,  0x80);                       /* DLAB on -> baud divisor */
    outb(COM1_DATA,       COM1_DIVISOR & 0xFF);        /* divisor low  (COM1_BAUD) */
    outb(COM1_INT_EN,     (COM1_DIVISOR >> 8) & 0xFF); /* divisor high */
    outb(COM1_LINE_CTRL,  0x03);                       /* 8N1, DLAB off */
    outb(COM1_FIFO_CTRL,  0xC7);  /* enable + clear FIFO, 14-byte threshold */
    outb(COM1_MODEM_CTRL, 0x0B);  /* DTR/RTS/OUT2 -- OUT2 gates IRQ delivery */
    outb(COM1_INT_EN,     0x00);  /* leave all UART IRQs masked until armed */
    s_serial_ready = true;
}

/* Toggle the THR-empty interrupt (only IRQ source we use). */
static inline void tx_set_etbei(bool on) {
    outb(COM1_INT_EN, on ? IER_ETBEI : 0x00);
}

/* Bounded-spin single-byte write. Used in sync mode and panic: blocks at
 * most SERIAL_SPIN_MAX iterations, then writes regardless (drops at most
 * the in-flight byte on a wedged UART) -- never hangs. */
static void serial_putc_sync(char c) {
    for (uint32_t s = 0; s < SERIAL_SPIN_MAX; s++) {
        if (inb(COM1_LINE_STAT) & LINE_STAT_THR_EMPTY) break;
        __asm__ volatile ("pause" ::: "memory");
    }
    outb(COM1_DATA, (uint8_t)c);
}

/* Drain the ring into the TX FIFO WITHOUT spinning. When THRE is set the
 * FIFO is empty, so we may write up to UART_FIFO_DEPTH bytes in one burst;
 * otherwise we write nothing and let the next THRE (IRQ or pump) continue.
 * Caller must hold g_tx_lock. Leaves ETBEI armed iff bytes remain. */
static void tx_drain_fifo_locked(void) {
    if (inb(COM1_LINE_STAT) & LINE_STAT_THR_EMPTY) {
        int budget = UART_FIFO_DEPTH;
        while (budget-- > 0 && !txr_empty()) {
            outb(COM1_DATA, (uint8_t)g_txr[g_txr_tail]);
            g_txr_tail = (g_txr_tail + 1u) & TXR_MASK;
        }
    }
    tx_set_etbei(!txr_empty());
}

void serial_putc(char c) {
    /* debugcon always on -- never blocks, never fails */
    dbgcon_putc(c);

    if (!s_serial_ready) serial_init();

    /* Sync mode: early boot (IRQ not yet armed) or panic. Bounded spin. */
    if (!s_tx_irq_mode) {
        serial_putc_sync(c);
        return;
    }

    /* Async mode: enqueue + kick the FIFO, never spin. */
    uint64_t flags = spin_lock_irqsave(&g_tx_lock);
    if (txr_full()) {
        /* Make room without spinning (one non-blocking FIFO top-up). If
         * still full, drop the byte -- responsiveness over a stall. The
         * dropped byte only loses log fidelity, never correctness. */
        tx_drain_fifo_locked();
        if (txr_full()) {
            g_txr_dropped++;      /* SILENT loss -- see serial_dropped() */
            spin_unlock_irqrestore(&g_tx_lock, flags);
            return;
        }
    }
    g_txr[g_txr_head] = c;
    g_txr_head = (g_txr_head + 1u) & TXR_MASK;
    tx_drain_fifo_locked();
    spin_unlock_irqrestore(&g_tx_lock, flags);
}

/* THRE interrupt: the TX FIFO emptied -- push the next burst. Stays silent
 * (no kprintf) so it can't re-enter serial_putc and self-deadlock on
 * g_tx_lock. */
static void serial_tx_irq(struct regs *r) {
    (void)r;
    (void)inb(COM1_IIR);   /* acknowledge / clear the THRE interrupt source */
    uint64_t flags = spin_lock_irqsave(&g_tx_lock);
    tx_drain_fifo_locked();
    spin_unlock_irqrestore(&g_tx_lock, flags);
    irq_eoi_isa(COM1_ISA_IRQ);
}

/* Idle-loop safety-net drain (pid 0). Guarantees the ring empties even if
 * IRQ4 never fires on this board. Cheap no-op when empty / in sync mode. */
void serial_tx_pump(void) {
    if (!s_tx_irq_mode || txr_empty()) return;
    uint64_t flags = spin_lock_irqsave(&g_tx_lock);
    tx_drain_fifo_locked();
    spin_unlock_irqrestore(&g_tx_lock, flags);
}

/* Arm async TX. Wire IRQ4 (works in both PIC and IO APIC modes), flip into
 * ring mode, then push whatever is already queued. Call once after the IRQ
 * facade is up (post irq_switch_to_ioapic). */
void serial_enable_tx_irq(void) {
    if (s_tx_irq_mode) return;
    if (!s_serial_ready) serial_init();
    irq_install_isa(COM1_ISA_IRQ, serial_tx_irq);   /* may kprintf (still sync) */
    s_tx_irq_mode = true;
    uint64_t flags = spin_lock_irqsave(&g_tx_lock);
    tx_drain_fifo_locked();
    spin_unlock_irqrestore(&g_tx_lock, flags);
}

/* Force synchronous (bounded-spin) output and flush the ring. Used by the
 * panic path, which runs with IRQs disabled and the scheduler dead -- the
 * THRE IRQ won't fire and the pump won't run, so the ring must be drained
 * inline to guarantee the full panic message reaches the wire. */
void serial_set_sync_mode(void) {
    /* Drain anything already queued, in order, via the bounded-spin path. */
    while (!txr_empty()) {
        char c = g_txr[g_txr_tail];
        g_txr_tail = (g_txr_tail + 1u) & TXR_MASK;
        serial_putc_sync(c);
    }
    tx_set_etbei(false);
    s_tx_irq_mode = false;
}

void serial_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) serial_putc(s[i]);
}

void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}
