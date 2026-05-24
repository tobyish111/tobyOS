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
bool rtc_is_ready(void);

#endif /* TOBYOS_RTC_H */
