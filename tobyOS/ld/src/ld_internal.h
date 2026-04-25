/* SPDX-License-Identifier: MIT
 *
 * tobyOS dynamic linker (ld-toby.so) — internal layout.
 *
 * Everything here is private to the dynamic linker itself. The
 * dynamic linker MUST NOT pull in libtoby; it must talk to the
 * kernel directly via SYSCALL.
 */
#ifndef LD_TOBY_INTERNAL_H
#define LD_TOBY_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

/* ---- ELF subset we care about -------------------------------- */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    int64_t  d_tag;
    uint64_t d_un;
} Elf64_Dyn;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_PHDR     6

#define DT_NULL      0
#define DT_NEEDED    1
#define DT_PLTRELSZ  2
#define DT_PLTGOT    3
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_STRSZ     10
#define DT_SYMENT    11
#define DT_PLTREL    20
#define DT_JMPREL    23
#define DT_RELACOUNT 0x6ffffff9

#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8

#define ELF64_R_TYPE(i)  ((uint32_t)((i) & 0xffffffff))
#define ELF64_R_SYM(i)   ((uint32_t)((i) >> 32))
#define ELF64_ST_BIND(i) ((i) >> 4)

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

#define SHN_UNDEF  0

/* ---- Auxiliary vector tags (mirror tobyos/abi/abi.h) --------- */

#define ABI_AT_NULL    0
#define ABI_AT_PHDR    3
#define ABI_AT_PHENT   4
#define ABI_AT_PHNUM   5
#define ABI_AT_PAGESZ  6
#define ABI_AT_BASE    7
#define ABI_AT_FLAGS   8
#define ABI_AT_ENTRY   9

struct abi_auxv {
    uint64_t a_type;
    uint64_t a_val;
};

/* ---- Kernel surface ------------------------------------------ */

#define ABI_SYS_WRITE       2
#define ABI_SYS_EXIT        1
#define ABI_SYS_ABI_VERSION 49
#define ABI_SYS_DLOAD       50

struct abi_dlmap_info {
    uint64_t base;
    uint64_t entry;
    uint64_t dynamic;
    uint64_t phdr;
    uint16_t phnum;
    uint16_t phent;
    uint32_t _pad;
};

long ld_syscall1(long n, long a1);
long ld_syscall3(long n, long a1, long a2, long a3);

static inline void ld_write(int fd, const void *buf, size_t n) {
    long a1 = fd, a2 = (long)(uintptr_t)buf, a3 = (long)n;
    ld_syscall3(ABI_SYS_WRITE, a1, a2, a3);
}

__attribute__((noreturn))
static inline void ld_exit(int code) {
    ld_syscall1(ABI_SYS_EXIT, code);
    for (;;) { __asm__ volatile ("hlt"); }
}

/* ---- Self relocation (asm-callable) -------------------------- */

void ld_self_relocate(uintptr_t self_base);

/* ---- C entry from _start ------------------------------------- */

uint64_t ld_main(uint64_t *user_stack_top, uintptr_t self_base);

/* ---- Tiny diagnostics (never printf) ------------------------- */

void ld_puts(const char *s);
void ld_die(const char *s) __attribute__((noreturn));

#endif
