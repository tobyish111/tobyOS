/* SPDX-License-Identifier: MIT
 *
 * Self-relocator for ld-toby.so.
 *
 * Runs FIRST, before any other C code in the linker. At this point
 * the loader's own .data and .rodata still hold link-time pointers
 * (the linker output was an ET_DYN, so every absolute pointer in
 * .data was emitted as an R_X86_64_RELATIVE relocation against
 * load_base==0). We must apply our own RELATIVE relocations in
 * place before anyone tries to dereference a global.
 *
 * Constraints (do not relax without thinking):
 *   - No global variable access except _DYNAMIC, which is hidden
 *     and resolved RIP-relative.
 *   - No string literals -- ld_puts() may dereference rodata, which
 *     hasn't been fixed up yet.
 *   - No memcpy/memset; the compiler must not emit calls to libc
 *     helpers. The body is small enough that -O2 won't.
 *   - No function calls into other TUs.
 *
 * The function is intentionally small and obvious.
 */

#include "ld_internal.h"

extern Elf64_Dyn _DYNAMIC[]
    __attribute__((visibility("hidden")));

void ld_self_relocate(uintptr_t base) {
    Elf64_Rela *rela   = 0;
    uint64_t    relasz = 0;

    for (Elf64_Dyn *d = _DYNAMIC; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA:
            rela   = (Elf64_Rela *)(base + d->d_un);
            break;
        case DT_RELASZ:
            relasz = d->d_un;
            break;
        default:
            break;
        }
    }

    if (!rela || !relasz) {
        /* No relocations? Then we have nothing to fix up. That's
         * fine; we just return. */
        return;
    }

    uint64_t n = relasz / sizeof(Elf64_Rela);
    for (uint64_t i = 0; i < n; i++) {
        if (ELF64_R_TYPE(rela[i].r_info) == R_X86_64_RELATIVE) {
            uint64_t *slot = (uint64_t *)(base + rela[i].r_offset);
            *slot = base + (uint64_t)rela[i].r_addend;
        }
        /* Anything else inside the linker itself is a build error.
         * We deliberately do NOT call ld_die() here, because the
         * string literal it would print isn't relocated yet. The
         * program will simply crash with a recognizable bad pointer
         * later, which is a strong enough signal at this stage. */
    }
}
