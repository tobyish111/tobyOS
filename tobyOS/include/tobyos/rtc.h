#ifndef TOBYOS_RTC_H
#define TOBYOS_RTC_H

#include <tobyos/types.h>

struct rtc_time {
    uint8_t  second;
    uint8_t  minute;
    uint8_t  hour;
    uint8_t  day;
    uint8_t  month;
    uint16_t year;
    uint8_t  weekday;
};

void rtc_init(void);
void rtc_read_time(struct rtc_time *t);
uint64_t rtc_unix_time(void);
/* Write the RTC, as UTC (hwclock -w, timesyncd). 0, or -1 for a time it
 * cannot hold. (finding 157) */
int rtc_write_time(const struct rtc_time *t);
/* ACPI's FADT century register index; 0 = none. (acpi.c) */
void rtc_set_century_reg(int reg);
/* /dev/rtc0's one opener: claim on open (false: EBUSY), release on close. */
bool rtc_dev_claim(void);
void rtc_dev_release(void);
bool rtc_is_ready(void);

#endif /* TOBYOS_RTC_H */
