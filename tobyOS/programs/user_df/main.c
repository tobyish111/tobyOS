#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fscheck_report {
    int  status;
    long total_blocks;
    long used_blocks;
    long free_blocks;
    long total_inodes;
    long used_inodes;
    int  errors_found;
    char msg[128];
};

static inline long sys_fs_check(const char *path, struct fscheck_report *out) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(64L), "D"((long)path), "S"((long)out)
        : "rcx", "r11", "memory");
    return r;
}

int main(int argc, char **argv) {
    const char *path = "/";
    if (argc > 1) path = argv[1];

    struct fscheck_report rpt;
    memset(&rpt, 0, sizeof(rpt));
    long r = sys_fs_check(path, &rpt);
    if (r < 0) {
        fprintf(stderr, "df: fs_check failed for '%s': error %ld\n", path, -r);
        return 1;
    }

    long used_pct = 0;
    if (rpt.total_blocks > 0)
        used_pct = (rpt.used_blocks * 100) / rpt.total_blocks;

    printf("Filesystem: %s\n", path);
    printf("%-12s %-12s %-12s %-6s\n", "Total", "Used", "Free", "Use%");
    printf("%-12ld %-12ld %-12ld %ld%%\n",
        rpt.total_blocks, rpt.used_blocks, rpt.free_blocks, used_pct);
    printf("\nInodes: %ld used / %ld total\n", rpt.used_inodes, rpt.total_inodes);

    if (rpt.errors_found)
        printf("WARNING: %d error(s) found: %s\n", rpt.errors_found, rpt.msg);

    return 0;
}
