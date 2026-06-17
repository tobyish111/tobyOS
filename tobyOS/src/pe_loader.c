/* pe_loader.c -- Windows PE/COFF (PE32+) executable loader (Track C, C1).
 *
 * Loads an unmodified Windows x86-64 .exe into a user address space and
 * makes it runnable on tobyOS. A Windows binary does NOT issue raw
 * syscalls -- it imports functions (kernel32!WriteFile, ...) through its
 * Import Address Table. So the loader's three jobs are:
 *
 *   1. Map the image. Copy the PE headers + every section to its virtual
 *      address (preferred ImageBase for C1), zero-fill BSS tails, then
 *      apply base relocations if we couldn't honour the preferred base.
 *
 *   2. Bridge the IAT to the kernel. The Win32 shims live in the kernel
 *      (src/syscall.c) but a PE runs at CPL3 and cannot call kernel code
 *      directly. So we map ONE user-mode page holding a shared
 *      "marshalling gate" plus a small per-import "thunk", and bind each
 *      IAT slot to its thunk. At runtime the PE does `call *[iat]` -> the
 *      thunk loads a shim index into eax and jmps the gate -> the gate
 *      captures the Microsoft-x64 argument registers and issues one
 *      ABI_SYS_WIN32_DISPATCH syscall into the kernel shim.
 *
 *   3. Tighten page permissions (W^X) to match each section.
 *
 * The actual Win32 API subset (GetStdHandle/WriteFile/ExitProcess for C1)
 * is implemented in src/syscall.c's win32 shim table; this file only
 * resolves names against it (win32_shim_index) and wires the thunks.
 *
 * Called from spawn (src/proc.c) with the child's CR3 + vmm editor root
 * already installed, exactly like elf_load_user_at. Writes into the freshly
 * mapped user pages go through a uaccess window so the loader is SMAP-safe.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/uaccess.h>
#include <tobyos/pe.h>
#include <tobyos/abi/abi.h>

/* ---- PE/COFF on-disk structures (all little-endian, packed) ---- */

struct dos_header {
    uint16_t e_magic;       /* "MZ" = 0x5A4D */
    uint16_t e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc;
    uint16_t e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid, e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;      /* file offset of the PE signature */
} __attribute__((packed));

struct coff_header {
    uint16_t machine;
    uint16_t num_sections;
    uint32_t timestamp;
    uint32_t sym_table_ptr;
    uint32_t num_symbols;
    uint16_t opt_header_size;
    uint16_t characteristics;
} __attribute__((packed));

struct pe_opt_header64 {
    uint16_t magic;              /* 0x020B = PE32+ */
    uint8_t  major_linker, minor_linker;
    uint32_t size_of_code, size_of_init_data, size_of_uninit_data;
    uint32_t entry_point_rva, base_of_code;
    uint64_t image_base;
    uint32_t section_alignment, file_alignment;
    uint16_t major_os_ver, minor_os_ver, major_image_ver, minor_image_ver;
    uint16_t major_subsys_ver, minor_subsys_ver;
    uint32_t win32_version, size_of_image, size_of_headers, checksum;
    uint16_t subsystem, dll_characteristics;
    uint64_t stack_reserve, stack_commit, heap_reserve, heap_commit;
    uint32_t loader_flags, num_data_dirs;
    /* num_data_dirs * struct pe_data_dir follow */
} __attribute__((packed));

struct pe_data_dir { uint32_t rva, size; } __attribute__((packed));

struct pe_section {
    char     name[8];
    uint32_t virtual_size, virtual_addr;
    uint32_t raw_data_size, raw_data_ptr;
    uint32_t reloc_ptr, linenum_ptr;
    uint16_t num_relocs, num_linenums;
    uint32_t characteristics;
} __attribute__((packed));

struct pe_import_desc {
    uint32_t original_first_thunk;   /* RVA to the Import Name Table */
    uint32_t timestamp, forwarder_chain;
    uint32_t name_rva;               /* RVA to the DLL name string */
    uint32_t first_thunk;            /* RVA to the IAT */
} __attribute__((packed));

struct pe_base_reloc { uint32_t page_rva, block_size; } __attribute__((packed));

#define PE_DIR_IMPORT      1
#define PE_DIR_BASERELOC   5

#define PE_MACHINE_AMD64   0x8664
#define PE_OPT_MAGIC_PE32P 0x020B

#define PE_RELOC_ABSOLUTE  0
#define PE_RELOC_DIR64     10

#define PE_SCN_MEM_EXECUTE 0x20000000u
#define PE_SCN_MEM_READ    0x40000000u
#define PE_SCN_MEM_WRITE   0x80000000u

#define PE_ORDINAL_FLAG    (1ULL << 63)

/* ---- Win32 marshalling thunk region ----
 *
 * One user page mapped into every PE process. Layout:
 *   [ gate (68 bytes) ][ thunk_0 (10) ][ thunk_1 ]...[ thunk_N ]
 * Placed above the user heap (USER_HEAP_BASE+max = 512 MiB) and below the
 * usual PE ImageBase (0x140000000), so it can't collide with either. */
#define WIN32_SHIM_BASE    0x0000000030000000ULL
#define WIN32_THUNK_SIZE   10
#define WIN32_GATE_IMM_OFF 0x51     /* offset of the syscall-number imm32 */

/* The Thread Environment Block. A Windows CRT reads gs:[0x30]
 * (NtCurrentTeb) during startup, so a PE runs CPL3 with its GS base = this
 * VA (see the SWAPGS scheme in proc.h/syscall_entry.S). One page, mapped
 * user-RW; only a few fields are meaningful to a minimal CRT. */
#define WIN32_TEB_BASE     0x0000000031000000ULL
#define WIN32_TEB_SELF     0x30    /* NtCurrentTeb: TEB's own linear address */
#define WIN32_TEB_STACKBASE  0x08
#define WIN32_TEB_STACKLIMIT 0x10
#define WIN32_USER_STACK_TOP 0x0000800000000000ULL

/* CRT data page (argc/argv/environ/_commode/_fmode): WIN32_CRT_DATA_BASE +
 * the layout offsets are in pe.h (shared with the shims in syscall.c). */

/* Position-independent marshalling gate (C2). Verified by assembling the
 * equivalent .intel_syntax and extracting .text (see the commit notes).
 * On entry: rax = shim index, rcx/rdx/r8/r9 = Microsoft-x64 args 0..3, and
 * stack args 4.. at [rsp+0x28], [rsp+0x30], ... (after the return address +
 * the 32-byte shadow space). We marshal all 8 into a contiguous array so
 * variadic Win32 functions (printf -> __stdio_common_vfprintf, whose
 * va_list is the 5th/stack arg) work.
 *
 *   push rdi ; push rsi            ; preserve MS-x64 non-volatile regs
 *   sub  rsp, 0x40                 ; 8-qword argument array
 *   mov  [rsp+00], rcx             ; arg0..arg3 from registers
 *   mov  [rsp+08], rdx
 *   mov  [rsp+10], r8
 *   mov  [rsp+18], r9
 *   mov  r10, [rsp+0x78] ; mov [rsp+20], r10   ; arg4 (orig [rsp+0x28]+0x50)
 *   mov  r10, [rsp+0x80] ; mov [rsp+28], r10   ; arg5
 *   mov  r10, [rsp+0x88] ; mov [rsp+30], r10   ; arg6
 *   mov  r10, [rsp+0x90] ; mov [rsp+38], r10   ; arg7
 *   mov  rdi, rax                  ; syscall a1 = shim index
 *   mov  rsi, rsp                  ; syscall a2 = &args
 *   mov  eax, <ABI_SYS_WIN32_DISPATCH>
 *   syscall
 *   add  rsp, 0x40 ; pop rsi ; pop rdi ; ret
 *
 * The a4..a7 reads come off the caller's stack; the PE's initial RSP is set
 * (in proc.c) to USER_STACK_TOP_VA-0x400 so these reads of up to +0x90 above
 * the call site stay inside the mapped stack even for the first call. For a
 * function with <4 stack args the surplus slots read harmless garbage the
 * shim ignores. */
static const uint8_t g_win32_gate[] = {
    0x57, 0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0x89, 0x0c, 0x24, 0x48, 0x89,
    0x54, 0x24, 0x08, 0x4c, 0x89, 0x44, 0x24, 0x10, 0x4c, 0x89, 0x4c, 0x24,
    0x18, 0x4c, 0x8b, 0x54, 0x24, 0x78, 0x4c, 0x89, 0x54, 0x24, 0x20, 0x4c,
    0x8b, 0x94, 0x24, 0x80, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x54, 0x24, 0x28,
    0x4c, 0x8b, 0x94, 0x24, 0x88, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x54, 0x24,
    0x30, 0x4c, 0x8b, 0x94, 0x24, 0x90, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x54,
    0x24, 0x38, 0x48, 0x89, 0xc7, 0x48, 0x89, 0xe6, 0xb8, 0x00, 0x00, 0x00,
    0x00, 0x0f, 0x05, 0x48, 0x83, 0xc4, 0x40, 0x5e, 0x5f, 0xc3
};
#define WIN32_GATE_SIZE (sizeof(g_win32_gate))

/* Shim-page layout (fixed offsets the loader + the kernel shims agree on):
 *   0x00  gate (94 bytes)
 *   0x80  thread wrapper (C6: CreateThread's CPL3 entry trampoline)
 *   0x100 per-import thunks (10 bytes each)
 * See WIN32_THREAD_WRAPPER_VA in pe.h for the wrapper's user VA. */
#define WIN32_WRAPPER_OFF   0x80
#define WIN32_THUNK_BASE    0x100
#define WIN32_WRAPPER_IMM_OFF 0x14    /* the thread-exit imm32 in the wrapper */

/* Position-independent thread wrapper (C6). A Win32 thread function is
 * entered here (via tobyOS thread_create, rdi = &{func, param}); it sets up
 * the Microsoft-x64 first arg (param -> rcx), calls the thread function,
 * then exits the thread with its DWORD return via a raw ABI_SYS_THREAD_EXIT
 * (patched into the imm32 at load). Verified by assembling the equivalent
 * .intel_syntax + extracting .text.
 *
 *   mov rax,[rdi] ; mov rcx,[rdi+8]     ; func, param(->rcx)
 *   sub rsp,0x20 ; call rax ; add rsp,0x20
 *   mov edi,eax ; mov eax,<THREAD_EXIT> ; syscall ; ud2
 */
static const uint8_t g_win32_thread_wrapper[] = {
    0x48, 0x8b, 0x07, 0x48, 0x8b, 0x4f, 0x08, 0x48, 0x83, 0xec, 0x20, 0xff,
    0xd0, 0x48, 0x83, 0xc4, 0x20, 0x89, 0xc7, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0x0f, 0x05, 0x0f, 0x0b
};
#define WIN32_WRAPPER_SIZE (sizeof(g_win32_thread_wrapper))

/* DispatchMessage trampoline (C7), at WIN32_DISPATCH_OFF. DispatchMessageA's
 * IAT slot points here. It reads the MSG (rcx), loads the registered WndProc
 * from the CRT-data slot (abs64 patched at load), and CALLS it in CPL3 with
 * the Microsoft-x64 args (hwnd, message, wParam, lParam) -- the kernel can't
 * call a user WndProc itself. Verified by assembling the equivalent asm.
 *
 *   sub rsp,0x28 ; mov rax,[rcx] ; mov r8,[rcx+0x10] ; mov r9,[rcx+0x18]
 *   mov edx,[rcx+8] ; mov rcx,rax ; movabs rax,<&wndproc> ; mov rax,[rax]
 *   test rax,rax ; jz 1f ; call rax ; 1: add rsp,0x28 ; ret
 */
#define WIN32_DISPATCH_OFF     0xC0
#define WIN32_DISPATCH_IMM_OFF 0x17    /* the &wndproc abs64 in the trampoline */
static const uint8_t g_win32_dispatch_stub[] = {
    0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x01, 0x4c, 0x8b, 0x41, 0x10, 0x4c,
    0x8b, 0x49, 0x18, 0x8b, 0x51, 0x08, 0x48, 0x89, 0xc1, 0x48, 0xb8, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x00, 0x48, 0x85,
    0xc0, 0x74, 0x02, 0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3
};
#define WIN32_DISPATCH_SIZE (sizeof(g_win32_dispatch_stub))

static inline uint64_t round_up(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

/* ---- MZ/PE sniff -------------------------------------------------------- */

bool pe_is_image(const void *image, size_t size) {
    if (!image || size < sizeof(struct dos_header)) return false;
    const struct dos_header *dos = (const struct dos_header *)image;
    if (dos->e_magic != 0x5A4D) return false;            /* "MZ" */
    uint64_t pe_off = dos->e_lfanew;
    if (pe_off + 4 > size) return false;
    const uint8_t *sig = (const uint8_t *)image + pe_off;
    return sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0;
}

/* Map [virt_lo, virt_hi) as RW + USER user pages (allocating frames). The
 * real per-section permissions are tightened later. Mirrors elf.c's
 * ensure_writable_pages. */
static bool pe_map_user(uint64_t virt_lo, uint64_t virt_hi) {
    uint32_t base = VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX;
    for (uint64_t v = virt_lo; v < virt_hi; v += PAGE_SIZE) {
        if (vmm_translate(v) != 0) {
            if (!vmm_protect(v, PAGE_SIZE, base)) return false;
            continue;
        }
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) return false;
        if (!vmm_map(v, phys, PAGE_SIZE, base)) { pmm_free_page(phys); return false; }
    }
    return true;
}

/* Return the shim-page VA of the user-mode stub for an import that must call
 * back into CPL3 (so it can't be a kernel shim), or 0 for the normal
 * gate-thunk path. Currently just DispatchMessage{A,W} -> the WndProc
 * trampoline. (func match is exact; the caller already knows the DLL is a
 * Win32 one.) */
static uint64_t win32_user_stub_va(const char *dll, const char *func) {
    (void)dll;
    if (strcmp(func, "DispatchMessageA") == 0 ||
        strcmp(func, "DispatchMessageW") == 0)
        return WIN32_DISPATCH_STUB_VA;
    return 0;
}

/* Build the gate + per-import thunks into the (already mapped, RW) shim
 * page, then bind every imported IAT slot to its thunk. All user-memory
 * access here is under the caller's uaccess window. `udir` points at the
 * data-directory array in the *original* (kernel) image buffer; the image
 * has already been copied to load_base so RVAs resolve in user memory. */
static bool pe_resolve_imports(uint64_t load_base, const struct pe_data_dir *udir,
                               uint32_t num_dirs) {
    /* Lay down the marshalling gate at the top of the shim page and patch
     * in the dispatch syscall number. */
    uint8_t gate[WIN32_GATE_SIZE];
    memcpy(gate, g_win32_gate, WIN32_GATE_SIZE);
    uint32_t nr = ABI_SYS_WIN32_DISPATCH;
    memcpy(&gate[WIN32_GATE_IMM_OFF], &nr, 4);

    /* Thread wrapper (C6) at its fixed offset, with the thread-exit syscall
     * number patched in. */
    uint8_t wrap[WIN32_WRAPPER_SIZE];
    memcpy(wrap, g_win32_thread_wrapper, WIN32_WRAPPER_SIZE);
    uint32_t tx = ABI_SYS_THREAD_EXIT;
    memcpy(&wrap[WIN32_WRAPPER_IMM_OFF], &tx, 4);

    /* DispatchMessage trampoline (C7), with the &wndproc CRT-data slot
     * patched into its abs64 load. */
    uint8_t disp[WIN32_DISPATCH_SIZE];
    memcpy(disp, g_win32_dispatch_stub, WIN32_DISPATCH_SIZE);
    uint64_t wndproc_slot = WIN32_CRT_DATA_BASE + WIN32_CRT_WNDPROC;
    memcpy(&disp[WIN32_DISPATCH_IMM_OFF], &wndproc_slot, 8);

    unsigned long uf = uaccess_begin();
    memcpy((void *)WIN32_SHIM_BASE, gate, WIN32_GATE_SIZE);
    memcpy((void *)(WIN32_SHIM_BASE + WIN32_WRAPPER_OFF), wrap, WIN32_WRAPPER_SIZE);
    memcpy((void *)(WIN32_SHIM_BASE + WIN32_DISPATCH_OFF), disp, WIN32_DISPATCH_SIZE);
    uaccess_end(uf);

    int next_thunk = 0;     /* next free thunk slot in the page */

    if (num_dirs <= PE_DIR_IMPORT) return true;     /* no imports at all */
    uint32_t import_rva = udir[PE_DIR_IMPORT].rva;
    if (!import_rva) return true;

    const struct pe_import_desc *imp =
        (const struct pe_import_desc *)(load_base + import_rva);

    unsigned long uf2 = uaccess_begin();
    for (; imp->name_rva; imp++) {
        const char *dll = (const char *)(load_base + imp->name_rva);

        /* INT (names) if present, else the IAT doubles as the name table. */
        uint64_t *iat = (uint64_t *)(load_base + imp->first_thunk);
        const uint64_t *names = imp->original_first_thunk
            ? (const uint64_t *)(load_base + imp->original_first_thunk)
            : iat;

        for (int k = 0; names[k]; k++) {
            uint64_t ent = names[k];
            int idx = -1;
            const char *fname = "<ordinal>";

            if (!(ent & PE_ORDINAL_FLAG)) {
                /* hint(2) + name(NUL-terminated) at RVA */
                fname = (const char *)(load_base + (uint32_t)ent + 2);
                idx = win32_shim_index(dll, fname);
            }

            /* Some imports are bound to a USER-MODE stub in the shim page
             * rather than a kernel gate-thunk, because they must call back
             * into CPL3 code (e.g. DispatchMessage -> the window's WndProc).
             * Bind those straight to the stub VA. */
            uint64_t stub = win32_user_stub_va(dll, fname);
            if (stub) {
                iat[k] = stub;
                kprintf("[pe]   bind %s!%s -> user-stub@%p\n",
                        dll, fname, (void *)stub);
                continue;
            }

            if (idx < 0) {
                /* Self-identifying gap, exactly like Track B's unhandled
                 * syscall log. The slot stays 0 -> calling it faults. */
                kprintf("[pe] unresolved import %s!%s\n", dll, fname);
                iat[k] = 0;
                continue;
            }

            /* Emit a 10-byte thunk:  mov eax,idx ; jmp gate */
            uint64_t thunk_va = WIN32_SHIM_BASE + WIN32_THUNK_BASE +
                                (uint64_t)next_thunk * WIN32_THUNK_SIZE;
            uint8_t t[WIN32_THUNK_SIZE];
            uint32_t imm = (uint32_t)idx;
            int32_t  rel = -(int32_t)(WIN32_THUNK_BASE +
                                      (uint32_t)next_thunk * WIN32_THUNK_SIZE +
                                      WIN32_THUNK_SIZE);
            t[0] = 0xB8; memcpy(&t[1], &imm, 4);    /* mov eax, idx */
            t[5] = 0xE9; memcpy(&t[6], &rel, 4);    /* jmp rel32 (gate) */
            memcpy((void *)thunk_va, t, WIN32_THUNK_SIZE);

            iat[k] = thunk_va;      /* bind the IAT slot */
            next_thunk++;

            kprintf("[pe]   bind %s!%s -> shim#%d thunk@%p\n",
                    dll, fname, idx, (void *)thunk_va);
        }
    }
    uaccess_end(uf2);
    return true;
}

int pe_load_user(const void *image, size_t size, int argc, char **argv,
                 struct pe_load_info *out) {
    if (!image || !out || !pe_is_image(image, size)) return -1;
    out->entry = 0;
    out->image_base = 0;

    const struct dos_header  *dos  = (const struct dos_header *)image;
    const uint8_t            *pehdr = (const uint8_t *)image + dos->e_lfanew;
    const struct coff_header *coff =
        (const struct coff_header *)(pehdr + 4);

    if ((uint64_t)dos->e_lfanew + 4 + sizeof(struct coff_header) > size) {
        kprintf("[pe] reject: COFF header runs off image\n");
        return -1;
    }
    if (coff->machine != PE_MACHINE_AMD64) {
        kprintf("[pe] reject: machine 0x%04x (need AMD64)\n", coff->machine);
        return -1;
    }

    const struct pe_opt_header64 *opt =
        (const struct pe_opt_header64 *)((const uint8_t *)coff + sizeof(*coff));
    if ((const uint8_t *)opt + sizeof(*opt) > (const uint8_t *)image + size) {
        kprintf("[pe] reject: optional header runs off image\n");
        return -1;
    }
    if (opt->magic != PE_OPT_MAGIC_PE32P) {
        kprintf("[pe] reject: not PE32+ (magic 0x%04x)\n", opt->magic);
        return -1;
    }

    uint64_t image_base    = opt->image_base;
    uint64_t size_of_image = round_up(opt->size_of_image, PAGE_SIZE);

    /* For C1 we map at the preferred ImageBase (delta == 0). The image must
     * fit in the user half and clear of the shim page. */
    if (image_base == 0 || image_base <= WIN32_SHIM_BASE ||
        image_base + size_of_image > 0x0000800000000000ULL) {
        kprintf("[pe] reject: ImageBase %p / size 0x%lx outside the usable "
                "user range\n", (void *)image_base, (unsigned long)size_of_image);
        return -1;
    }
    uint64_t load_base = image_base;
    int64_t  delta     = (int64_t)(load_base - image_base);   /* 0 for C1 */

    const struct pe_data_dir *dirs = (const struct pe_data_dir *)
        ((const uint8_t *)opt + sizeof(*opt));
    const struct pe_section *secs = (const struct pe_section *)
        ((const uint8_t *)opt + coff->opt_header_size);

    kprintf("[pe] PE32+ base=%p size=0x%lx entry_rva=0x%x sections=%u dirs=%u\n",
            (void *)load_base, (unsigned long)size_of_image,
            opt->entry_point_rva, coff->num_sections, opt->num_data_dirs);

    /* ---- 1. map the image span + the shim page (RW, user) ---- */
    if (!pe_map_user(load_base, load_base + size_of_image)) {
        kprintf("[pe] failed mapping image pages\n");
        return -1;
    }
    if (!pe_map_user(WIN32_SHIM_BASE, WIN32_SHIM_BASE + PAGE_SIZE)) {
        kprintf("[pe] failed mapping shim page\n");
        return -1;
    }
    if (!pe_map_user(WIN32_TEB_BASE, WIN32_TEB_BASE + PAGE_SIZE)) {
        kprintf("[pe] failed mapping TEB page\n");
        return -1;
    }
    if (!pe_map_user(WIN32_CRT_DATA_BASE, WIN32_CRT_DATA_BASE + PAGE_SIZE)) {
        kprintf("[pe] failed mapping CRT data page\n");
        return -1;
    }

    /* ---- minimal TEB ---- the CRT startup reads gs:[0x30] (NtCurrentTeb).
     * Zero the page, then set the self-pointer + stack bounds. The proc's GS
     * base is set to WIN32_TEB_BASE by the spawn path, and the SWAPGS scheme
     * makes gs:[...] resolve here while the PE runs in CPL3. */
    {
        unsigned long uf = uaccess_begin();
        memset((void *)WIN32_TEB_BASE, 0, PAGE_SIZE);
        *(uint64_t *)(WIN32_TEB_BASE + WIN32_TEB_SELF)       = WIN32_TEB_BASE;
        *(uint64_t *)(WIN32_TEB_BASE + WIN32_TEB_STACKBASE)  = WIN32_USER_STACK_TOP;
        *(uint64_t *)(WIN32_TEB_BASE + WIN32_TEB_STACKLIMIT) = WIN32_USER_STACK_TOP - 0x200000ULL;
        uaccess_end(uf);
    }

    /* ---- CRT data page: argc / argv[] / environ[] / _commode / _fmode ----
     * The ucrt startup reads these via __p_* accessor shims. Copy the kernel-
     * side argv strings into the user page and build the pointer arrays. */
    {
        uint64_t base = WIN32_CRT_DATA_BASE;
        unsigned long uf = uaccess_begin();
        memset((void *)base, 0, PAGE_SIZE);

        int n = (argc > 0) ? argc : 0;
        /* cap so the arrays + strings stay on the page */
        if (n > 16) n = 16;
        uint64_t argv_arr = base + WIN32_CRT_ARGV_ARR;
        uint64_t strp     = base + WIN32_CRT_STRINGS;
        uint64_t strp_end = base + PAGE_SIZE;
        for (int i = 0; i < n; i++) {
            const char *s = argv ? argv[i] : 0;
            if (!s) { ((uint64_t *)argv_arr)[i] = 0; continue; }
            ((uint64_t *)argv_arr)[i] = strp;
            while (*s && strp < strp_end - 1) *(char *)(strp++) = *s++;
            *(char *)(strp++) = '\0';
        }
        ((uint64_t *)argv_arr)[n] = 0;                 /* argv[argc] = NULL */

        *(int32_t *)(base + WIN32_CRT_ARGC)    = n;
        *(int32_t *)(base + WIN32_CRT_COMMODE) = 0;
        *(int32_t *)(base + WIN32_CRT_FMODE)   = 0;
        *(uint64_t *)(base + WIN32_CRT_ARGV)        = argv_arr;
        *(uint64_t *)(base + WIN32_CRT_ENVIRON)     = base + WIN32_CRT_ENVIRON_ARR;
        *(uint64_t *)(base + WIN32_CRT_ENVIRON_ARR) = 0;   /* environ[0] = NULL */

        /* C7: _acmdln (the raw command line) -> the program name. The GUI CRT
         * startup reads it via __p__acmdln to build WinMain's lpCmdLine. */
        {
            const char *s0 = (n > 0 && argv) ? argv[0] : "";
            uint64_t cl = base + WIN32_CRT_CMDLINE;
            uint64_t clp = cl;
            while (*s0 && clp < base + PAGE_SIZE - 1) *(char *)(clp++) = *s0++;
            *(char *)clp = '\0';
            *(uint64_t *)(base + WIN32_CRT_ACMDLN) = cl;   /* char* to the cmdline */
        }

        /* C4: a minimal "C"-locale lconv. Only decimal_point (the first
         * field) needs to be a valid non-NULL "."; the rest stay zeroed,
         * which the CRT reads as "no grouping / empty". Plus a strerror msg. */
        *(char *)(base + WIN32_CRT_LCONV_DOT)     = '.';
        *(char *)(base + WIN32_CRT_LCONV_DOT + 1) = '\0';
        *(uint64_t *)(base + WIN32_CRT_LCONV)     = base + WIN32_CRT_LCONV_DOT; /* decimal_point */
        memcpy((void *)(base + WIN32_CRT_STRERR), "error", 6);
        uaccess_end(uf);
    }

    /* ---- 2. copy headers + sections into the mapped pages ---- */
    {
        unsigned long uf = uaccess_begin();
        uint32_t hdr = opt->size_of_headers;
        if (hdr > size) hdr = (uint32_t)size;
        memcpy((void *)load_base, image, hdr);

        for (uint16_t i = 0; i < coff->num_sections; i++) {
            const struct pe_section *s = &secs[i];
            uint64_t va = load_base + s->virtual_addr;
            uint32_t fsz = s->raw_data_size;
            if (fsz && (uint64_t)s->raw_data_ptr + fsz <= size) {
                memcpy((void *)va, (const uint8_t *)image + s->raw_data_ptr, fsz);
            } else {
                fsz = 0;
            }
            if (s->virtual_size > fsz) {
                memset((void *)(va + fsz), 0, s->virtual_size - fsz);
            }
        }
        uaccess_end(uf);
    }

    /* ---- 3. base relocations (only if we couldn't honour ImageBase) ---- */
    if (delta != 0 && opt->num_data_dirs > PE_DIR_BASERELOC &&
        dirs[PE_DIR_BASERELOC].rva && dirs[PE_DIR_BASERELOC].size) {
        unsigned long uf = uaccess_begin();
        const uint8_t *rp  = (const uint8_t *)(load_base + dirs[PE_DIR_BASERELOC].rva);
        const uint8_t *end = rp + dirs[PE_DIR_BASERELOC].size;
        while (rp + sizeof(struct pe_base_reloc) <= end) {
            const struct pe_base_reloc *blk = (const struct pe_base_reloc *)rp;
            if (blk->block_size < sizeof(*blk)) break;
            uint32_t n = (blk->block_size - sizeof(*blk)) / 2;
            const uint16_t *e = (const uint16_t *)(rp + sizeof(*blk));
            for (uint32_t j = 0; j < n; j++) {
                if ((e[j] >> 12) == PE_RELOC_DIR64) {
                    uint64_t *patch = (uint64_t *)(load_base + blk->page_rva + (e[j] & 0xFFF));
                    *patch += (uint64_t)delta;
                }
            }
            rp += blk->block_size;
        }
        uaccess_end(uf);
    }

    /* ---- 4. bind the IAT to the Win32 shim thunks ---- */
    if (!pe_resolve_imports(load_base, dirs, opt->num_data_dirs)) {
        kprintf("[pe] failed resolving imports\n");
        return -1;
    }

    /* ---- 5. tighten W^X: per-section perms + R/NX headers + R-X shim ---- */
    (void)vmm_protect(load_base, PAGE_SIZE, VMM_PRESENT | VMM_USER | VMM_NX);
    for (uint16_t i = 0; i < coff->num_sections; i++) {
        const struct pe_section *s = &secs[i];
        if (s->virtual_size == 0) continue;
        uint64_t lo = load_base + (s->virtual_addr & ~(PAGE_SIZE - 1));
        uint64_t hi = load_base + round_up((uint64_t)s->virtual_addr + s->virtual_size,
                                           PAGE_SIZE);
        uint32_t f = VMM_PRESENT | VMM_USER;
        if (s->characteristics & PE_SCN_MEM_WRITE)     f |= VMM_WRITE;
        if (!(s->characteristics & PE_SCN_MEM_EXECUTE)) f |= VMM_NX;
        (void)vmm_protect(lo, hi - lo, f);
    }
    /* shim page: executable, read-only (no write, no NX) for the PE at CPL3 */
    (void)vmm_protect(WIN32_SHIM_BASE, PAGE_SIZE, VMM_PRESENT | VMM_USER);
    /* TEB + CRT data pages: data, read-write, never executable */
    (void)vmm_protect(WIN32_TEB_BASE, PAGE_SIZE, VMM_PRESENT | VMM_USER | VMM_WRITE | VMM_NX);
    (void)vmm_protect(WIN32_CRT_DATA_BASE, PAGE_SIZE, VMM_PRESENT | VMM_USER | VMM_WRITE | VMM_NX);

    out->image_base = load_base;
    out->entry      = load_base + opt->entry_point_rva;
    out->teb        = WIN32_TEB_BASE;
    out->crt_data   = WIN32_CRT_DATA_BASE;
    kprintf("[pe] loaded: base=%p entry=%p (gate@%p teb@%p crt@%p)\n",
            (void *)load_base, (void *)out->entry,
            (void *)WIN32_SHIM_BASE, (void *)WIN32_TEB_BASE,
            (void *)WIN32_CRT_DATA_BASE);
    return 0;
}
