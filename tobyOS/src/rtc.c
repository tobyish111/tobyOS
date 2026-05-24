#include <tobyos/rtc.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>

static bool g_rtc_ready;

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static void wait_for_update(void) {
    while (cmos_read(0x0A) & 0x80) {}
}

void rtc_read_time(struct rtc_time *t) {
    if (!t) return;

    wait_for_update();

    uint8_t status_b = cmos_read(0x0B);
    bool is_binary = (status_b & 0x04) != 0;
    bool is_24h    = (status_b & 0x02) != 0;

    t->second  = cmos_read(0x00);
    t->minute  = cmos_read(0x02);
    uint8_t raw_hour = cmos_read(0x04);
    t->hour    = raw_hour;
    t->weekday = cmos_read(0x06);
    t->day     = cmos_read(0x07);
    t->month   = cmos_read(0x08);
    uint8_t year_low = cmos_read(0x09);
    uint8_t century  = cmos_read(0x32);

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
