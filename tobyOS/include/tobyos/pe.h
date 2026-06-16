/* pe.h -- Windows PE/COFF (PE32+) executable loader + Win32 shim glue.
 *
 * Track C (Win32 app compatibility). A Windows .exe does NOT make raw
 * syscalls the way a Linux binary does; it imports functions from DLLs
 * (kernel32, ...) through its Import Address Table (IAT). So loading one
 * is three jobs:
 *   1. map the image's sections + apply base relocations  (pe_load_user)
 *   2. bind each imported symbol to a tobyOS-provided shim  (the IAT
 *      thunks, installed by pe_load_user)
 *   3. implement a Win32 API subset on top of tobyOS primitives  (the
 *      kernel-side shim table + dispatcher, in src/syscall.c)
 *
 * The shims run in the KERNEL, but a Windows process runs at CPL3 and
 * cannot call kernel code directly. The loader bridges this with a
 * user-mode marshalling gate: each IAT slot points at a tiny user-mode
 * thunk that funnels into one shared gate, which captures the
 * Microsoft-x64 argument registers and issues a single
 * ABI_SYS_WIN32_DISPATCH syscall (see abi.h).
 */
#ifndef TOBYOS_PE_H
#define TOBYOS_PE_H

#include <tobyos/types.h>

struct pe_load_info {
    uint64_t entry;        /* user VA of the PE entry point (AddressOfEntryPoint + base) */
    uint64_t image_base;   /* actual load base (== preferred base for C1) */
};

/* True if [image,size) looks like a PE/COFF image: 'MZ' DOS magic and a
 * 'PE\0\0' signature at e_lfanew. Cheap sniff used by the exec path to
 * branch ELF vs PE. */
bool pe_is_image(const void *image, size_t size);

/* Map a PE32+ image into the *currently active* user address space. The
 * caller (spawn) must already have installed the child's CR3 + vmm editor
 * root so vmm_map edits + user writes land in the child. Applies base
 * relocations, installs the Win32 IAT thunks + marshalling gate, and
 * reports the entry point. Returns 0 on success, <0 on failure. */
int pe_load_user(const void *image, size_t size, struct pe_load_info *out);

/* Resolve "dll!func" to the kernel-side Win32 shim index used to bind an
 * IAT thunk (case-insensitive dll match, exact func match). Returns the
 * index, or -1 if the symbol is not provided. Defined alongside the shim
 * table in src/syscall.c. */
int win32_shim_index(const char *dll, const char *func);

/* The ABI_SYS_WIN32_DISPATCH target: invoke shim `func_index` with the
 * 8-qword argument array at user VA `args_ptr`. Returns the Win32
 * function's result. Defined in src/syscall.c. */
long win32_dispatch(uint64_t func_index, uint64_t args_ptr);

#endif /* TOBYOS_PE_H */
