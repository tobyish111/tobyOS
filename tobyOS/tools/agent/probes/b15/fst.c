#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
int main(void) {
    struct stat st;
    int r = fstat(0, &st);
    printf("fstat0 r=%d errno=%d mode=0%o isreg=%d isfifo=%d\n", r, r ? errno : 0,
           (unsigned)st.st_mode, S_ISREG(st.st_mode), S_ISFIFO(st.st_mode));
    fd_set rd; FD_ZERO(&rd); FD_SET(0, &rd);
    struct timespec ts = { 1, 500000000 };
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    int s = pselect(1, &rd, 0, 0, &ts, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("pselect r=%d errno=%d ms=%ld\n", s, s < 0 ? errno : 0,
           (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000));
    return 0;
}
