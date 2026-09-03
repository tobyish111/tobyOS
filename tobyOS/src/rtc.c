#include <tobyos/rtc.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>

static bool g_rtc_ready;

/* THE CMOS IS ONE INDEX/DATA PORT PAIR SHARED BY EVERY CPU (2026-09-02).
 *
 * cmos_read writes the register number to 0x70 and reads the value from
 * 0x71. There is no per-CPU state, so two CPUs interleaving those two steps
 * each read the register the OTHER one selected. That was harmless while the
 * only callers were rtc_init and a handful of kernel paths that never ran
 * concurrently. It stopped being harmless the moment /proc/stat started
 * reading the clock, because any process on any core could then race the
 * kernel's own RTC use -- and a garbled read of register 0x0A with bit 7 set
 * sent wait_for_update() below into a spin it could never leave.
 *
 * Found on real 4-core hardware, where `head -1 /proc/stat` wedged the
 * machine every time. QEMU never reproduced it: its RTC answers instantly
 * and the gate reads /proc/stat from one core at a time. */
static spinlock_t g_cmos_lock = SPINLOCK_INIT;

/* Caller must hold g_cmos_lock. */
static uint8_t cmos_read_locked(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

/* BOUNDED, and that is the whole point. This was:
 *
 *     while (cmos_read(0x0A) & 0x80) {}
 *
 * an unbounded spin on a bit in hardware we do not control, reached from a
 * /proc read with the big kernel lock held. One bad read wedged the entire
 * machine: the spinning CPU never released the BKL, every other CPU piled up
 * in bkl_enter, and pid 0's service lane -- which drives every network
 * service and the GUI -- stopped running. From outside, the box still
 * answered ping and completed TCP handshakes, because those come off the NIC
 * interrupt, and never said another word.
 *
 * The update-in-progress flag is set for roughly 2 ms once per second, so
 * this cap is many times the honest worst case. Past it the hardware is
 * misbehaving, and slightly stale time beats a dead kernel.
 *
 * Caller must hold g_cmos_lock. Returns false if it gave up. */
#define CMOS_UIP_SPINS 50000

static bool wait_for_update_locked(void) {
    for (int i = 0; i < CMOS_UIP_SPINS; i++) {
        if (!(cmos_read_locked(0x0A) & 0x80)) return true;
        __asm__ volatile ("pause" ::: "memory");
    }
    return false;
}

void rtc_read_time(struct rtc_time *t) {
    if (!t) return;

    /* IRQ-safe: the RTC is also touched from the periodic interrupt, and a
     * CPU holding this lock must not be interrupted into taking it again. */
    uint64_t flags = spin_lock_irqsave(&g_cmos_lock);

    if (!wait_for_update_locked()) {
        static int warned;
        if (warned < 3) {
            warned++;
            kprintf("[rtc] update-in-progress stuck after %d reads -- "
                    "reading anyway; time may be off by a second\n",
                    CMOS_UIP_SPINS);
        }
    }

    uint8_t status_b = cmos_read_locked(0x0B);
    bool is_binary = (status_b & 0x04) != 0;
    bool is_24h    = (status_b & 0x02) != 0;

    t->second  = cmos_read_locked(0x00);
    t->minute  = cmos_read_locked(0x02);
    uint8_t raw_hour = cmos_read_locked(0x04);
    t->hour    = raw_hour;
    t->weekday = cmos_read_locked(0x06);
    t->day     = cmos_read_locked(0x07);
    t->month   = cmos_read_locked(0x08);
    uint8_t year_low = cmos_read_locked(0x09);
    uint8_t century  = cmos_read_locked(0x32);

    /* Every port access is done: release before the arithmetic below, which
     * touches no hardware. */
    spin_unlock_irqrestore(&g_cmos_lock, flags);

    if (!is_binary) {
        t->second  = bcd_to_bin(t->second);
        t->minute  = bcd_to_bin(t->minute);
        t->hour    = bcd_to_bin(t->hour & 0x7F);
        t->weekday = bcd_to_bin(t->weekday);
        t->day     = bcd_to_bin(t->day);
        t->month   = bcd_to_bin(t->month);
        year_low   = bcd_to_bin(year_low);
        century    = bcd_to_bin(century);
    }

    if (!is_24h && (raw_hour & 0x80)) {
        t->hour = (t->hour % 12) + 12;
    }

    t->year = (century ? (uint16_t)century * 100 : 2000) + year_low;
}

uint64_t rtc_unix_time(void) {
    struct rtc_time t;
    rtc_read_time(&t);

    static const uint16_t mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    uint64_t y = t.year;
    uint64_t days = 0;

    for (uint64_t i = 1970; i < y; i++) {
        days += 365;
        if ((i % 4 == 0 && i % 100 != 0) || (i % 400 == 0)) days++;
    }

    if (t.month >= 1 && t.month <= 12)
        days += mdays[t.month - 1];

    if (t.month > 2) {
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) days++;
    }

    days += t.day - 1;

    return days * 86400ULL + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
}

bool rtc_is_ready(void) {
    return g_rtc_ready;
}

void rtc_init(void) {
    struct rtc_time t;
    rtc_read_time(&t);
    g_rtc_ready = true;

    kprintf("[rtc] %04u-%02u-%02u %02u:%02u:%02u (CMOS clock)\n",
            t.year, t.month, t.day, t.hour, t.minute, t.second);
}
