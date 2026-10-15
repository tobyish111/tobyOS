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

/* Caller must hold g_cmos_lock. */
static void cmos_write_locked(uint8_t reg, uint8_t v) {
    outb(0x70, reg);
    outb(0x71, v);
}

static uint8_t bin_to_bcd(unsigned v) {
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/* THE CENTURY REGISTER IS THE FADT'S (ACPI's RTC century index), as Linux
 * takes it: 0 means the RTC has none. It was read at 0x32 on every machine
 * -- on a board where that byte is a BIOS setting, the year came out as
 * that byte's hundreds -- and writing it there would change the setting.
 * -1 until ACPI is parsed: reads keep using 0x32 then, writes leave the
 * century alone. (finding 157, the write-back) */
static int g_century_reg = -1;
void rtc_set_century_reg(int reg) { g_century_reg = reg; }

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
    int creg = g_century_reg < 0 ? 0x32 : g_century_reg;
    uint8_t century  = creg ? cmos_read_locked((uint8_t)creg) : 0;

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
    } else {
        t->hour    = raw_hour & 0x7F;     /* the PM bit is not part of it */
    }

    /* A 12-hour RTC counts 12, 1, ..., 11 with bit 7 for PM: 12 AM is hour
     * 0 and 12 PM is 12. 12 AM read as 12. */
    if (!is_24h) {
        t->hour = (uint8_t)(t->hour % 12 + ((raw_hour & 0x80) ? 12 : 0));
    }

    /* No century register: the two-digit year pivots at 70, as Linux's
     * mc146818 reading does (it was always 20xx). */
    if (century)
        t->year = (uint16_t)century * 100 + year_low;
    else
        t->year = (uint16_t)(year_low < 70 ? 2000 + year_low : 1900 + year_low);
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

/* /dev/rtc0 has one opener at a time, as Linux's (rtc_dev_open's
 * RTC_DEV_BUSY): a second open is EBUSY. */
static int g_rtc_dev_busy;
bool rtc_dev_claim(void) {
    return __atomic_exchange_n(&g_rtc_dev_busy, 1, __ATOMIC_ACQ_REL) == 0;
}
void rtc_dev_release(void) {
    __atomic_store_n(&g_rtc_dev_busy, 0, __ATOMIC_RELEASE);
}

bool rtc_is_ready(void) {
    return g_rtc_ready;
}

/* Write the RTC (hwclock -w, timesyncd's write-back: finding 157). The time
 * is UTC, the RTC's timescale here as it is a Linux system's by default.
 * As Linux's mc146818_set_time: the update cycle held (register B's SET
 * bit) and the divider in reset (register A's DV bits 11x), every field is
 * written in the RTC's own format -- BCD or binary, 12- or 24-hour, as
 * register B says -- and both let go. The divider coming out of reset puts
 * the next tick 500 ms after the write, so a write of second S at S + 0.5
 * keeps the RTC in step with the system clock (hwclock and timesyncd write
 * then). On AMD and Hygon, register A's bit 4 selects the CMOS bank and is
 * left alone there, as Linux leaves it: the tick keeps its old phase.
 * 0, or -1 for a time it cannot hold: 1970..2199 with a century register,
 * 1970..2069 (the two-digit pivot) without. */
int rtc_write_time(const struct rtc_time *t) {
    if (!t || t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
        t->hour > 23 || t->minute > 59 || t->second > 59)
        return -1;
    int creg = g_century_reg > 0 ? g_century_reg : 0;
    if (t->year < 1970 || t->year > (creg ? 2199 : 2069)) return -1;

    /* the day of the week, which the RTC keeps 1..7 from Sunday */
    int64_t y = t->year, m = t->month;
    if (m <= 2) { y--; m += 12; }
    int64_t days = 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 +
                   t->day - 719469;                     /* days since 1970-01-01 */
    unsigned wday = (unsigned)(((days % 7) + 7 + 4) % 7);   /* 0 Sunday */

    /* AMD's and Hygon's CPUID vendor strings, "AuthenticAMD" and
     * "HygonGenuine": their first four bytes, in EBX */
    uint32_t va, vb, vc, vd;
    __asm__ volatile ("cpuid" : "=a"(va), "=b"(vb), "=c"(vc), "=d"(vd) : "a"(0), "c"(0));
    bool amd = vb == 0x68747541u || vb == 0x6f677948u;

    uint64_t flags = spin_lock_irqsave(&g_cmos_lock);
    (void)wait_for_update_locked();
    uint8_t b = cmos_read_locked(0x0B);
    bool bin = (b & 0x04) != 0, h24 = (b & 0x02) != 0;
#define RTC_ENC(v) (bin ? (uint8_t)(v) : bin_to_bcd((unsigned)(v)))
    uint8_t hour;
    if (h24) {
        hour = RTC_ENC(t->hour);
    } else {
        unsigned h = t->hour % 12u;
        hour = (uint8_t)(RTC_ENC(h ? h : 12u) | (t->hour >= 12 ? 0x80 : 0));
    }
    cmos_write_locked(0x0B, (uint8_t)(b | 0x80));   /* SET: hold the update cycle */
    uint8_t a = (uint8_t)(cmos_read_locked(0x0A) & 0x7F);   /* bit 7, UIP, is read-only */
    if (!amd)
        cmos_write_locked(0x0A, (uint8_t)(a | 0x70));   /* the divider in reset */
    cmos_write_locked(0x00, RTC_ENC(t->second));
    cmos_write_locked(0x02, RTC_ENC(t->minute));
    cmos_write_locked(0x04, hour);
    cmos_write_locked(0x06, RTC_ENC(wday + 1));
    cmos_write_locked(0x07, RTC_ENC(t->day));
    cmos_write_locked(0x08, RTC_ENC(t->month));
    cmos_write_locked(0x09, RTC_ENC(t->year % 100));
    if (creg) cmos_write_locked((uint8_t)creg, RTC_ENC(t->year / 100));
    cmos_write_locked(0x0B, (uint8_t)(b & ~0x80));
    if (!amd) cmos_write_locked(0x0A, a);              /* and running: tick in 500 ms */
#undef RTC_ENC
    spin_unlock_irqrestore(&g_cmos_lock, flags);
    return 0;
}

void rtc_init(void) {
    struct rtc_time t;
    rtc_read_time(&t);
    g_rtc_ready = true;

    kprintf("[rtc] %04u-%02u-%02u %02u:%02u:%02u (CMOS clock)\n",
            t.year, t.month, t.day, t.hour, t.minute, t.second);
}
