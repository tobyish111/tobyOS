/* elf.h -- minimal ELF64 loader for trusted programs.
 *
 * "Trusted" means: the only ELFs we ever load are ones we built ourselves
 * and shipped in the ISO via Limine's module mechanism. Two flavours:
 *
 *   elf_load          (a.k.a. elf_load_kernel)
 *      vaddrs must be in the kernel half (>= 0xFFFF800000000000)
 *      pages mapped without VMM_USER -- the program runs at CPL=0
 *      on the kernel stack, in the kernel address space. Use elf_run
 *      to call into it directly from C; see programs/hello/.
 *
 *   elf_load_user     (milestone 3D)
 *      vaddrs must be in the canonical lower half (>0, <0x800000000000)
 *      pages mapped with VMM_USER set -- so user_load_and_run can
 *      transition to ring 3 via enter_user_mode after providing a
 *      stack. There is no kernel-side "call entry()" wrapper for
 *      user mode; the asm trampoline does the iretq.
 *
 * Both variants share validation + segment-walk machinery and only
 * differ in the address-space restriction and the VMM_USER bit. PIE,
 * dynamic linking, relocations, interpreter requests, and TLS are all
 * rejected up front.
 */

#ifndef TOBYOS_ELF_H
#define TOBYOS_ELF_H

#include <tobyos/types.h>

/* ---- ELF64 on-disk structures (subset we actually inspect) ---- */

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;

#define EI_NIDENT 16

#define EI_MAG0    0
#define EI_MAG1    1
#define EI_MAG2    2
#define EI_MAG3    3
#define EI_CLASS   4
#define EI_DATA    5
#define EI_VERSION 6
#define EI_OSABI   7        /* target OS/ABI -- selects the process personality */

/* EI_OSABI values we care about. SysV(0) is what our own toolchain emits
 * for native binaries; LINUX(3) (== GNU) is the FreeBSD-style "brandelf"
 * tag that opts a binary into the Linux x86-64 syscall ABI. */
#define ELFOSABI_SYSV   0
#define ELFOSABI_LINUX  3
#define ELFOSABI_GNU    3

#define ELFMAG0    0x7f
#define ELFMAG1    'E'
#define ELFMAG2    'L'
#define ELFMAG3    'F'

#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define EV_CURRENT   1

#define ET_NONE  0
#define ET_REL   1
#define ET_EXEC  2
#define ET_DYN   3

#define EM_X86_64 62

typedef struct {
    uint8_t    e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff;
    Elf64_Off  e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6
#define PT_TLS     7

/* GNU OS-specific program headers (the 0x6474e55x "GNU_*" range). These are
 * emitted by GNU/LLVM toolchains targeting an OS triple (x86_64-*-linux-*) and
 * are absent from bare-metal (x86_64-elf) output -- which is what tobyOS's own
 * native programs build as. PT_GNU_STACK in particular is emitted on EVERY
 * Linux-target binary, static or dynamic, branded or not, even under
 * -nostdlib. That makes it the one signal that distinguishes a note-less,
 * static, unbranded SYSV *Linux* binary from a native tobyOS static binary
 * (see elf_is_linux_abi / B19). */
#define PT_GNU_EH_FRAME 0x6474e550u
#define PT_GNU_STACK    0x6474e551u
#define PT_GNU_RELRO    0x6474e552u
#define PT_GNU_PROPERTY 0x6474e553u

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

/* ---- ELF64 section header (needed by the kernel module loader) ---- */

typedef int64_t  Elf64_Sxword;

typedef struct {
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

/* Section types. */
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_NOBITS      8

/* Section flags. */
#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4

/* ---- ELF64 symbol table entry ---- */

typedef struct {
    Elf64_Word    st_name;
    uint8_t       st_info;
    uint8_t       st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

#define ELF64_ST_BIND(i)   ((i) >> 4)
#define ELF64_ST_TYPE(i)   ((i) & 0xf)
#define ELF64_ST_INFO(b,t) (((b) << 4) + ((t) & 0xf))

#define STB_LOCAL   0
#define STB_GLOBAL  1

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3

#define SHN_UNDEF   0
#define SHN_ABS     0xFFF1

/* ---- ELF64 relocation with addend ---- */

typedef struct {
    Elf64_Addr    r_offset;
    Elf64_Xword   r_info;
    Elf64_Sxword  r_addend;
} Elf64_Rela;

#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xFFFFFFFF)

/* x86_64 relocation types used by kernel modules. */
#define R_X86_64_NONE    0
#define R_X86_64_64      1
#define R_X86_64_PC32    2
#define R_X86_64_PLT32   4
#define R_X86_64_32S     11

/* ---- loader API ---- */

/* Calling convention for loaded programs: zero-arg, returns int (in
 * RAX). They run in kernel mode on the kernel's stack -- so they share
 * everything with the kernel and must be considered trusted. */
typedef int (*elf_entry_t)(void);

/* Validate `image[0..size)` and load every PT_LOAD segment into the
 * kernel address space at the addresses the ELF's program headers
 * dictate. On success writes the entry point to *out_entry and returns
 * true; on any validation failure returns false (and may have allocated
 * partial pages, which we leak -- the kernel module set is fixed at
 * boot, so leakage is bounded). */
bool elf_load(const void *image, size_t size, elf_entry_t *out_entry);

/* Same as elf_load but for user-mode programs:
 *   - vaddrs must be in the canonical lower half (NULL page rejected),
 *   - leaf PTEs are minted with VMM_USER set,
 *   - intermediate PML4/PDPT/PD entries get US=1 (vmm_map handles this
 *     when VMM_USER is in the flag set).
 * The entry pointer is returned as a uintptr_t (you can't call it from
 * C -- you must enter ring 3 first via enter_user_mode).
 *
 * Backwards-compat wrapper around elf_load_user_at(image, size, 0, ...)
 * that returns only the entry point. Rejects ET_DYN -- callers needing
 * dynamic loading must use elf_load_user_at directly. */
bool elf_load_user(const void *image, size_t size, uint64_t *out_entry);

/* Milestone 25D: extended user-side loader.
 *
 * Accepts both ET_EXEC (load_base must be 0) and ET_DYN (load_base
 * picks the base address; every p_vaddr is biased by load_base before
 * mapping). Writes back enough information for an in-process dynamic
 * linker (or the kernel auxv packer) to find the loaded program's
 * PHDR table:
 *
 *   out->entry      = e_entry + (ET_DYN ? load_base : 0)
 *   out->load_base  = load_base passed in
 *   out->phdr_va    = runtime VA of the program-header table (uses
 *                      PT_PHDR if present, else best-effort
 *                      load_base + e_phoff when e_phoff falls inside
 *                      a PT_LOAD).
 *   out->phnum      = e_phnum
 *   out->phent      = e_phentsize
 *   out->has_interp = true if a PT_INTERP segment was seen
 *
 * No memory is allocated for the caller; phdr_va just points into
 * already-loaded user pages. */
struct elf_load_info {
    uint64_t entry;
    uint64_t load_base;
    uint64_t phdr_va;
    uint16_t phnum;
    uint16_t phent;
    bool     has_interp;
    uint8_t  osabi;        /* e_ident[EI_OSABI] -- drives ABI personality */
    bool     has_gnu_phdr; /* B19: a PT_GNU_STACK/RELRO/PROPERTY phdr was seen
                            * -- a GNU/Linux toolchain fingerprint that native
                            * x86_64-elf binaries never carry. */
};

bool elf_load_user_at(const void *image, size_t size, uint64_t load_base,
                      struct elf_load_info *out);

/* Milestone 25D: peek at PT_INTERP without loading anything. Used by
 * the kernel spawn path to discover the dynamic-linker pathname so it
 * can read THAT image off the VFS and load it alongside the program.
 *
 * On success copies up to `cap-1` bytes of the interpreter path into
 * `out`, NUL-terminates, and returns true. Returns false if the ELF is
 * malformed, has no PT_INTERP, or the interp string would not fit in
 * `cap`. */
bool elf_peek_interp(const void *image, size_t size, char *out, size_t cap);

/* Decide the ABI personality for an ELF: true => Linux, false => native tobyOS.
 * Positive Linux evidence, in order:
 *   1. OSABI brand == ELFOSABI_LINUX (FreeBSD-style `brandelf` tag).
 *   2. PT_INTERP names a known Linux dynamic loader (ld-linux* / ld-musl*) --
 *      unbranded dynamic Linux binaries drop-and-run.
 *   3. (B19) A *static* (no PT_INTERP) binary with a GNU OS-specific program
 *      header (PT_GNU_STACK/RELRO/PROPERTY) -- catches the last gap: note-less,
 *      static, unbranded SYSV Linux binaries, which still carry PT_GNU_STACK
 *      from their Linux-target toolchain. Native *static* tobyOS (x86_64-elf)
 *      binaries never do; the !has_interp guard exempts native *dynamic*
 *      programs (ld-toby.so), which do carry it but are classified by rule 2.
 * Conservative: native is the default. */
bool elf_is_linux_abi(uint8_t osabi, bool has_interp, const char *interp_path,
                      bool has_gnu_phdr);

/* Convenience: elf_load + entry(); the program's int return is written
 * to *out_rc (if non-NULL). Returns false if loading fails. Only valid
 * for kernel-mode ELFs. */
bool elf_run(const void *image, size_t size, int *out_rc);

#endif /* TOBYOS_ELF_H */
