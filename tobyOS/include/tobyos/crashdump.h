#ifndef TOBYOS_CRASHDUMP_H
#define TOBYOS_CRASHDUMP_H

#include <tobyos/types.h>

struct crash_info {
    uint64_t rip, rsp, rbp;
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t cr2, cr3;
    uint32_t error_code;
    char     message[128];
    char     process_name[32];
    int      pid;
};

void crashdump_init(void);
void crashdump_save(const struct crash_info *info);
void crashdump_display(const struct crash_info *info);

#endif /* TOBYOS_CRASHDUMP_H */
