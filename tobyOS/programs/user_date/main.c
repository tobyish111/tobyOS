#include <stdio.h>
#include <stdlib.h>

static inline long sys_clock_ms(void) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(48L)
        : "rcx", "r11", "memory");
    return r;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    long ms = sys_clock_ms();
    if (ms < 0) {
        fprintf(stderr, "date: clock syscall failed\n");
        return 1;
    }

    long total_sec = ms / 1000;
    long days = total_sec / 86400;
    long rem = total_sec % 86400;
    long hours = rem / 3600;
    rem %= 3600;
    long minutes = rem / 60;
    long seconds = rem % 60;

    printf("System uptime: ");
    if (days > 0)
        printf("%ld day%s, ", days, days == 1 ? "" : "s");
    printf("%02ld:%02ld:%02ld\n", hours, minutes, seconds);
    printf("(%ld ms since boot)\n", ms);

    return 0;
}
