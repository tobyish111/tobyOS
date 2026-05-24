/* crash_recovery.h -- handle kernel panics and unclean shutdowns. */

#ifndef TOBYOS_CRASH_RECOVERY_H
#define TOBYOS_CRASH_RECOVERY_H

#include <tobyos/types.h>

#define CRASH_MAGIC 0xDEADC0DE

struct crash_dump {
    uint32_t magic;
    uint32_t timestamp;
    char     message[256];
    uint64_t rip, rsp, rbp;
    uint64_t regs[16];
    char     backtrace[512];
    uint32_t checksum;
};

void crash_recovery_init(void);
void crash_save_dump(const struct crash_dump *dump);
int  crash_load_dump(struct crash_dump *out);
void crash_clear(void);
int  crash_check_unclean_shutdown(void);
void crash_set_clean_shutdown(void);
void crash_set_dirty_flag(void);

#endif /* TOBYOS_CRASH_RECOVERY_H */
