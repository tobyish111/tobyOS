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
#include <tobyos/perf.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/proc.h>
#include <tobyos/mmap.h>

/* IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE: the PE opted into ASLR (C24). */
#define PE_DLLCHAR_DYNAMIC_BASE 0x0040u

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

#define PE_DIR_EXPORT      0   /* W3: a DLL's export directory */
#define PE_DIR_IMPORT      1
#define PE_DIR_RESOURCE    2   /* C14: .rsrc (RT_DIALOG dialog templates) */
#define PE_DIR_BASERELOC   5
#define PE_DIR_TLS         9   /* C18b: .tls (IMAGE_TLS_DIRECTORY64 -- __thread) */

/* C18b: the PE TLS directory (data-dir index 9). All four addresses are
 * absolute VAs already biased by the preferred ImageBase (subject to base
 * relocation); for C1 delta==0 so they equal the loaded VA. The compiler emits
 *   mov eax,[_tls_index]; mov rcx,gs:[0x58]; mov rsi,[rcx+rax*8]
 * for a _Thread_local/__declspec(thread) access, so the loader must point
 * gs:[0x58] (ThreadLocalStoragePointer) at a per-thread pointer array whose
 * [_tls_index] slot holds this module's TLS block (template raw + zero-fill). */
struct pe_tls_dir64 {
    uint64_t raw_start;     /* StartAddressOfRawData (VA of the template bytes) */
    uint64_t raw_end;       /* EndAddressOfRawData                              */
    uint64_t index_addr;    /* AddressOfIndex (VA where _tls_index is written)  */
    uint64_t callbacks;     /* AddressOfCallBacks (VA of NULL-terminated array) */
    uint32_t zero_fill;     /* SizeOfZeroFill (extra zero bytes after raw)      */
    uint32_t characteristics;
} __attribute__((packed));

#define PE_MACHINE_AMD64   0x8664
#define PE_FILE_DLL        0x2000  /* COFF characteristics: IMAGE_FILE_DLL */
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
/* C18a: the shim region grew from 1 to 2 pages -- the procaddr table (one 10-byte
 * thunk per shim) crossed the 4 KiB page once the shim count passed ~300.
 * W1: 6 pages -- the procaddr table may fill pages 1-3 and pages 4-5 hold the
 * missing-import stubs (layout in pe.h). */
#define WIN32_SHIM_BYTES   (WIN32_SHIM_PAGES * PAGE_SIZE)
#define WIN32_THUNK_SIZE   10
#define WIN32_GATE_IMM_OFF 0x6e     /* offset of the syscall-number imm32 (C11: 10-arg gate) */

/* The Thread Environment Block. A Windows CRT reads gs:[0x30]
 * (NtCurrentTeb) during startup, so a PE runs CPL3 with its GS base = this
 * VA (see the SWAPGS scheme in proc.h/syscall_entry.S). One page, mapped
 * user-RW; only a few fields are meaningful to a minimal CRT. */
#define WIN32_TEB_BASE     0x0000000031000000ULL
#define WIN32_TEB_SELF     0x30    /* NtCurrentTeb: TEB's own linear address */
#define WIN32_TEB_STACKBASE  0x08
#define WIN32_TEB_STACKLIMIT 0x10
#define WIN32_TEB_TLSPTR     0x58  /* C18b: x64 ThreadLocalStoragePointer */
#define WIN32_USER_STACK_TOP 0x0000800000000000ULL

/* C18b: the main thread's TLS region (one or more pages). Layout:
 *   [ pointer array (WIN32_TLS_BLK_OFF bytes) ][ this module's TLS block ]
 * gs:[0x58] -> WIN32_TLS_BASE (the array); array[_tls_index] -> the block.
 * Per-thread CreateThread threads get their own array+block from the Win32
 * heap (see w32_CreateThread); only the main thread uses this fixed region. */
#define WIN32_TLS_BASE     0x0000000031002000ULL
#define WIN32_TLS_BLK_OFF  0x80    /* the pointer array occupies [0..0x80) */
#define WIN32_TLS_MAX_BLK  (3u * PAGE_SIZE)   /* cap a single module's static TLS */

/* CRT data page (argc/argv/environ/_commode/_fmode): WIN32_CRT_DATA_BASE +
 * the layout offsets are in pe.h (shared with the shims in syscall.c). */

/* Position-independent marshalling gate (C2, extended at C11 from 8 to 10 args).
 * On entry: rax = shim index, rcx/rdx/r8/r9 = Microsoft-x64 args 0..3, and stack
 * args 4.. at [rsp+0x28], [rsp+0x30], ... (after the return address + the 32-byte
 * shadow space). We marshal a0..a9 into a contiguous array so variadic Win32
 * functions (printf's va_list = the 5th/stack arg) AND >8-arg functions
 * (CreateWindowExA's hWndParent = a8) work.
 *
 *   push rdi ; push rsi            ; preserve MS-x64 non-volatile regs
 *   sub  rsp, 0x50                 ; 10-qword argument array
 *   mov  [rsp+00], rcx             ; arg0..arg3 from registers
 *   mov  [rsp+08], rdx ; ...+10, r8 ; ...+18, r9
 *   mov  r10, [rsp+0x88] ; mov [rsp+20], r10   ; arg4 (orig [rsp+0x28]+0x60)
 *   ... a5 [rsp+0x90], a6 [rsp+0x98], a7 [rsp+0xA0], a8 [rsp+0xA8], a9 [rsp+0xB0]
 *   mov  rdi, rax                  ; syscall a1 = shim index
 *   mov  rsi, rsp                  ; syscall a2 = &args (10 qwords)
 *   mov  eax, <ABI_SYS_WIN32_DISPATCH>  ; imm32 at WIN32_GATE_IMM_OFF
 *   syscall
 *   add  rsp, 0x50 ; pop rsi ; pop rdi ; ret
 *
 * The a4..a9 reads come off the caller's stack; the PE's initial RSP is set
 * (in proc.c) to USER_STACK_TOP_VA-0x400 so these reads of up to +0xB0 above
 * the call site stay inside the mapped stack even for the first call. For a
 * function with fewer stack args the surplus slots read harmless garbage the
 * shim ignores. */
/* C11: extended to marshal 10 args (a0..a9), so CreateWindowExA's hWndParent
 * (its 9th arg = a8) reaches the shim -- needed for child/owned windows. Frame
 * is `sub rsp,0x50` (10 qwords); a4..a9 read off the caller stack at
 * [rsp+0x88..0xB0] (the orig [rsp+0x28..] + 0x60 push/sub shift). 123 bytes,
 * still under the thread wrapper at 0x80. Assembled offline + verified by
 * objdump; the syscall-number imm32 sits at WIN32_GATE_IMM_OFF (0x6e). */
static const uint8_t g_win32_gate[] = {
    0x57, 0x56, 0x48, 0x83, 0xec, 0x50, 0x48, 0x89, 0x0c, 0x24, 0x48, 0x89,
    0x54, 0x24, 0x08, 0x4c, 0x89, 0x44, 0x24, 0x10, 0x4c, 0x89, 0x4c, 0x24,
    0x18, 0x4c, 0x8b, 0x94, 0x24, 0x88, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x54,
    0x24, 0x20, 0x4c, 0x8b, 0x94, 0x24, 0x90, 0x00, 0x00, 0x00, 0x4c, 0x89,
    0x54, 0x24, 0x28, 0x4c, 0x8b, 0x94, 0x24, 0x98, 0x00, 0x00, 0x00, 0x4c,
    0x89, 0x54, 0x24, 0x30, 0x4c, 0x8b, 0x94, 0x24, 0xa0, 0x00, 0x00, 0x00,
    0x4c, 0x89, 0x54, 0x24, 0x38, 0x4c, 0x8b, 0x94, 0x24, 0xa8, 0x00, 0x00,
    0x00, 0x4c, 0x89, 0x54, 0x24, 0x40, 0x4c, 0x8b, 0x94, 0x24, 0xb0, 0x00,
    0x00, 0x00, 0x4c, 0x89, 0x54, 0x24, 0x48, 0x48, 0x89, 0xc7, 0x48, 0x89,
    0xe6, 0xb8, 0xef, 0xbe, 0xad, 0xde, 0x0f, 0x05, 0x48, 0x83, 0xc4, 0x50,
    0x5e, 0x5f, 0xc3
};
#define WIN32_GATE_SIZE (sizeof(g_win32_gate))

/* Shim-page layout (fixed offsets the loader + the kernel shims agree on):
 *   0x00  gate (123 bytes, C11: 10-arg)
 *   0x80  (free: C6's thread wrapper, until W5)
 *   0xC0  DispatchMessage trampoline (C7)
 *   0x100 dialog modal-loop trampoline (C14: DialogBoxParamA, 106 bytes)
 *   0x1F0 dialog-service gate-thunk (C14: __toby_dlgsvc, 10 bytes)
 *   0x200 (free: W3's init trampoline, until W5)
 *   0x280 fail entry, 0x2C0 _execute_onexit_table (W3)
 *   0x300 the loader's stubs (W4, W5, W7: 766 bytes): LoadLibrary, FreeLibrary,
 *         ExitThread, ExitProcess, thread and process start, the CRT's exit
 *   0xE00 setjmp / 0xE80 longjmp leaf stubs (C19b)
 *   0x1000.. procaddr table (one thunk per shim); 0x4000.. missing-import stubs
 * See WIN32_LDR_STUBS_VA / WIN32_DLG_TRAMP_VA in pe.h for the user VAs. */
#define WIN32_DLGTRAMP_OFF  0x100     /* C14: DialogBoxParamA trampoline */
#define WIN32_DLGSVC_THUNK_OFF 0x1F0  /* C14: __toby_dlgsvc gate-thunk (fixed VA) */
#define WIN32_THUNK_BASE    0x200

/* C9: per-import thunks grow up from WIN32_THUNK_BASE; the GetProcAddress thunk
 * table (one slot per shim) sits at this fixed offset above them. Both must stay
 * within the single 4 KiB shim page. */
#define WIN32_PROCADDR_OFF  ((uint64_t)(WIN32_PROCADDR_BASE_VA - WIN32_SHIM_BASE))

/* Emit a 10-byte marshalling thunk (`mov eax,idx ; jmp gate`) at `page_off` in
 * the shim page. The jmp is rel32 to the gate at offset 0. Caller holds the
 * uaccess window. */
/* C20: byte offset of the float-return gate within the shim page (no dependency
 * on the gate blob, so it can sit ahead of pe_emit_gate_thunk). */
#define WIN32_FGATE_OFF  ((uint64_t)(WIN32_FGATE_VA - WIN32_SHIM_BASE))

static void pe_emit_gate_thunk(uint64_t page_off, uint32_t idx) {
    uint8_t t[WIN32_THUNK_SIZE];
    /* C20: a double-returning shim routes through the float-return gate (which
     * marshals xmm0..xmm3 and puts the result back in xmm0); everything else
     * uses the integer gate at offset 0. */
    uint64_t target_off = win32_shim_is_double_ret((int)idx) ? WIN32_FGATE_OFF : 0;
    int32_t rel = (int32_t)((int64_t)target_off - (int64_t)(page_off + WIN32_THUNK_SIZE));
    t[0] = 0xB8; memcpy(&t[1], &idx, 4);    /* mov eax, idx */
    t[5] = 0xE9; memcpy(&t[6], &rel, 4);    /* jmp rel32 (gate) */
    memcpy((void *)(WIN32_SHIM_BASE + page_off), t, WIN32_THUNK_SIZE);
}

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

/* C14: the DialogBoxParamA modal-loop trampoline (at WIN32_DLGTRAMP_OFF).
 * DialogBoxParamA's IAT slot points here. It creates the dialog (kernel),
 * then runs a GetMessage/DispatchMessage loop in CPL3 -- PUMP-ing one message
 * at a time from the kernel and dispatching each to the app's DialogProc via
 * the C7 DispatchMessage trampoline -- until EndDialog, then returns the
 * EndDialog result. Reaches the kernel through the __toby_dlgsvc gate-thunk at
 * the FIXED VA 0x300001F0, and DispatchMessage at the FIXED VA 0x300000C0, so
 * the blob needs no load-time patching. Assembled offline (.tmp_c14a/dlgtramp.S
 * -> clang --target=x86_64-elf -> objdump/objcopy) + verified.
 *
 *   push rbx ; sub rsp,0x60
 *   ; BEGIN: dlgsvc(0, hInst, template, parent, dlgProc, initParam)
 *   ; loop: r = dlgsvc(1, &msg) ; if !r goto done ; DispatchMessage(&msg) ; loop
 *   ; done: rax = dlgsvc(2) ; add rsp,0x60 ; pop rbx ; ret
 */
static const uint8_t g_win32_dlg_tramp[] = {
    0x53, 0x48, 0x83, 0xec, 0x60, 0x4c, 0x89, 0xc8, 0x4d, 0x89, 0xc1, 0x49,
    0x89, 0xd0, 0x48, 0x89, 0xca, 0x31, 0xc9, 0x48, 0x89, 0x44, 0x24, 0x20,
    0x4c, 0x8b, 0x94, 0x24, 0x90, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x54, 0x24,
    0x28, 0x48, 0xc7, 0xc0, 0xf0, 0x01, 0x00, 0x30, 0xff, 0xd0, 0xb9, 0x01,
    0x00, 0x00, 0x00, 0x48, 0x8d, 0x54, 0x24, 0x30, 0x48, 0xc7, 0xc0, 0xf0,
    0x01, 0x00, 0x30, 0xff, 0xd0, 0x48, 0x85, 0xc0, 0x74, 0x10, 0x48, 0x8d,
    0x4c, 0x24, 0x30, 0x48, 0xc7, 0xc0, 0xc0, 0x00, 0x00, 0x30, 0xff, 0xd0,
    0xeb, 0xd8, 0xb9, 0x02, 0x00, 0x00, 0x00, 0x48, 0xc7, 0xc0, 0xf0, 0x01,
    0x00, 0x30, 0xff, 0xd0, 0x48, 0x83, 0xc4, 0x60, 0x5b, 0xc3,
};
#define WIN32_DLG_TRAMP_SIZE (sizeof(g_win32_dlg_tramp))

/* C19b: pure-CPL3 setjmp/longjmp leaf stubs (no syscall). A real binary that
 * uses setjmp/longjmp (e.g. Lua's protected-call error handling) binds those
 * IAT slots here. The jmp_buf layout is private + self-consistent between the
 * two stubs (Windows _JBLEN=256 bytes is plenty): offsets
 *   +0 rbx  +8 rbp  +0x10 rsp(caller)  +0x18 r12  +0x20 r13  +0x28 r14
 *   +0x30 r15  +0x38 rsi  +0x40 rdi  +0x48 return-address
 * No SEH unwinding (sufficient for a plain non-local goto on the same stack).
 *
 * setjmp(rcx=buf [, rdx=frame ignored]) -> 0  (mingw __intrinsic_setjmp passes
 * a frame ptr in rdx; we don't unwind, so it's unused):
 *   mov [rcx],rbx ; mov [rcx+8],rbp ; lea rax,[rsp+8] ; mov [rcx+0x10],rax
 *   mov [rcx+0x18],r12 ; ... r15 ; mov [rcx+0x38],rsi ; mov [rcx+0x40],rdi
 *   mov rax,[rsp] ; mov [rcx+0x48],rax ; xor eax,eax ; ret
 */
static const uint8_t g_win32_setjmp_stub[] = {
    0x48,0x89,0x19, 0x48,0x89,0x69,0x08, 0x48,0x8d,0x44,0x24,0x08,
    0x48,0x89,0x41,0x10, 0x4c,0x89,0x61,0x18, 0x4c,0x89,0x69,0x20,
    0x4c,0x89,0x71,0x28, 0x4c,0x89,0x79,0x30, 0x48,0x89,0x71,0x38,
    0x48,0x89,0x79,0x40, 0x48,0x8b,0x04,0x24, 0x48,0x89,0x41,0x48,
    0x31,0xc0, 0xc3
};
#define WIN32_SETJMP_STUB_SIZE (sizeof(g_win32_setjmp_stub))

/* longjmp(rcx=buf, edx=val) -> never returns to here; resumes at the setjmp
 * call site with eax = (val ? val : 1):
 *   mov rbx,[rcx] ; mov rbp,[rcx+8] ; mov r12,[rcx+0x18] ; ... r15
 *   mov rsi,[rcx+0x38] ; mov rdi,[rcx+0x40] ; mov eax,edx ; test eax,eax
 *   jnz 1f ; mov eax,1 ; 1: mov rsp,[rcx+0x10] ; jmp [rcx+0x48]
 */
static const uint8_t g_win32_longjmp_stub[] = {
    0x48,0x8b,0x19, 0x48,0x8b,0x69,0x08, 0x4c,0x8b,0x61,0x18,
    0x4c,0x8b,0x69,0x20, 0x4c,0x8b,0x71,0x28, 0x4c,0x8b,0x79,0x30,
    0x48,0x8b,0x71,0x38, 0x48,0x8b,0x79,0x40, 0x89,0xd0, 0x85,0xc0,
    0x75,0x05, 0xb8,0x01,0x00,0x00,0x00, 0x48,0x8b,0x61,0x10,
    0xff,0x61,0x48
};
#define WIN32_LONGJMP_STUB_SIZE (sizeof(g_win32_longjmp_stub))
#define WIN32_SETJMP_OFF   ((uint64_t)(WIN32_SETJMP_STUB_VA  - WIN32_SHIM_BASE))
#define WIN32_LONGJMP_OFF  ((uint64_t)(WIN32_LONGJMP_STUB_VA - WIN32_SHIM_BASE))

/* C20: the float-return marshalling gate. Like the integer gate, but it also
 * captures the four MS-x64 FP-arg registers xmm0..xmm3 (as bit patterns) into
 * the argument array AFTER the four integer regs (so a[0..3]=rcx/rdx/r8/r9 and
 * a[4..7]=xmm0..xmm3), and after the dispatch syscall returns the result's bit
 * pattern in rax it does `movq xmm0, rax` so a `double` return lands in xmm0.
 * A double-returning shim's thunk jmps here (see pe_emit_gate_thunk). Assembled
 * offline (.tmp_c20/fgate.S -> clang --target=x86_64-elf -> objdump) + verified;
 * the dispatch imm32 sits at WIN32_FGATE_IMM_OFF.
 *
 *   push rdi ; push rsi ; sub rsp,0x40
 *   mov [rsp+00],rcx ; +08,rdx ; +10,r8 ; +18,r9
 *   movq [rsp+20],xmm0 ; +28,xmm1 ; +30,xmm2 ; +38,xmm3
 *   mov rdi,rax ; mov rsi,rsp ; mov eax,<DISPATCH> ; syscall
 *   movq xmm0,rax ; add rsp,0x40 ; pop rsi ; pop rdi ; ret
 */
#define WIN32_FGATE_IMM_OFF 0x38
static const uint8_t g_win32_fgate[] = {
    0x57, 0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0x89, 0x0c, 0x24, 0x48, 0x89,
    0x54, 0x24, 0x08, 0x4c, 0x89, 0x44, 0x24, 0x10, 0x4c, 0x89, 0x4c, 0x24,
    0x18, 0x66, 0x0f, 0xd6, 0x44, 0x24, 0x20, 0x66, 0x0f, 0xd6, 0x4c, 0x24,
    0x28, 0x66, 0x0f, 0xd6, 0x54, 0x24, 0x30, 0x66, 0x0f, 0xd6, 0x5c, 0x24,
    0x38, 0x48, 0x89, 0xc7, 0x48, 0x89, 0xe6, 0xb8, 0xef, 0xbe, 0xad, 0xde,
    0x0f, 0x05, 0x66, 0x48, 0x0f, 0x6e, 0xc0, 0x48, 0x83, 0xc4, 0x40, 0x5e,
    0x5f, 0xc3
};
#define WIN32_FGATE_SIZE (sizeof(g_win32_fgate))

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
    /* C14: a modal dialog runs its message loop in CPL3 (it calls the app's
     * DialogProc), so DialogBoxParamA is bound to the dialog trampoline. */
    if (strcmp(func, "DialogBoxParamA") == 0)
        return WIN32_DLG_TRAMP_VA;
    /* C19b: setjmp/longjmp run entirely in CPL3 (save/restore the caller's
     * context); mingw emits the setjmp side as __intrinsic_setjmp. */
    if (strcmp(func, "__intrinsic_setjmp") == 0 ||
        strcmp(func, "_setjmp") == 0 || strcmp(func, "setjmp") == 0)
        return WIN32_SETJMP_STUB_VA;
    if (strcmp(func, "longjmp") == 0)
        return WIN32_LONGJMP_STUB_VA;
    /* W3: running a DLL CRT's onexit table calls the program's functions. */
    if (strcmp(func, "_execute_onexit_table") == 0)
        return WIN32_ONEXIT_EXEC_VA;
    /* W4: LoadLibrary and FreeLibrary call DllMain. */
    if (strcmp(func, "LoadLibraryA") == 0)   return WIN32_LDR_LOADA_VA;
    if (strcmp(func, "LoadLibraryW") == 0)   return WIN32_LDR_LOADW_VA;
    if (strcmp(func, "LoadLibraryExA") == 0) return WIN32_LDR_LOADEXA_VA;
    if (strcmp(func, "LoadLibraryExW") == 0) return WIN32_LDR_LOADEXW_VA;
    if (strcmp(func, "FreeLibrary") == 0)    return WIN32_LDR_FREE_VA;
    /* W5: ending a thread or the process calls the DLLs' DllMains and TLS
     * callbacks (DLL_THREAD_DETACH, DLL_PROCESS_DETACH) -- the CRT's exit
     * and _exit too, which end in ExitProcess on Windows. */
    if (strcmp(func, "ExitThread") == 0 || strcmp(func, "_endthreadex") == 0)
        return WIN32_LDR_EXITTHR_VA;
    if (strcmp(func, "FreeLibraryAndExitThread") == 0) return WIN32_LDR_FREEEXIT_VA;
    if (strcmp(func, "ExitProcess") == 0 || strcmp(func, "_exit") == 0)
        return WIN32_LDR_EXITPROC_VA;
    /* W7: the CRT's exit runs the .exe's atexit functions (_crt_atexit's
     * list: its C++ static destructors) first; _exit and ExitProcess do not */
    if (strcmp(func, "exit") == 0)           return WIN32_LDR_CRTEXIT_VA;
    return 0;
}

/* W1: the missing-import stubs (pages 4-5 of the shim area; layout in pe.h).
 * Each is
 *   mov rcx, <user VA of "DLL!function">   ; 48 B9 imm64
 *   mov eax, <__toby_missing shim index>    ; B8 imm32
 *   jmp gate                                ; E9 rel32
 * and lands in w32_toby_missing (syscall.c), which names the function and
 * ends the process. Nothing the program passed matters: the call does not
 * return. Caller holds the uaccess window. */
struct pe_missing_area {
    uint64_t next_stub, next_name;
    int      idx;                  /* __toby_missing's shim index, or -1 */
};

static void pe_emit_missing_stub(uint64_t stub_va, uint64_t name_va, int idx) {
    uint8_t t[WIN32_MISSING_STUB_SIZE];
    uint32_t i32 = (uint32_t)idx;
    int32_t rel = (int32_t)((int64_t)WIN32_SHIM_BASE -
                            (int64_t)(stub_va + WIN32_MISSING_STUB_SIZE));
    t[0]  = 0x48; t[1] = 0xB9; memcpy(&t[2], &name_va, 8);   /* mov rcx, imm64 */
    t[10] = 0xB8; memcpy(&t[11], &i32, 4);                   /* mov eax, imm32 */
    t[15] = 0xE9; memcpy(&t[16], &rel, 4);                   /* jmp rel32 gate */
    memcpy((void *)stub_va, t, sizeof(t));
}

/* Stub 0 is the shared fallback for when the stub or the name area is full;
 * its text names no function. */
static void pe_missing_init(struct pe_missing_area *m) {
    static const char fallback[] = "an import (the missing-import area is full)";
    m->idx = win32_shim_index("kernel32.dll", "__toby_missing");
    m->next_stub = WIN32_MISSING_STUB_VA;
    m->next_name = WIN32_MISSING_NAMES_VA;
    if (m->idx < 0) return;
    memcpy((void *)m->next_name, fallback, sizeof(fallback));
    pe_emit_missing_stub(m->next_stub, m->next_name, m->idx);
    m->next_name += sizeof(fallback);
    m->next_stub += WIN32_MISSING_STUB_SIZE;
}

/* The VA an import nothing implements is bound to: its own stub naming
 * "dll!func", or stub 0 when an area is full, or 0 if the kernel has no
 * __toby_missing shim (the pre-W1 behaviour). */
static uint64_t pe_missing_stub(struct pe_missing_area *m, const char *dll,
                                const char *func) {
    if (m->idx < 0) return 0;
    size_t dl = strlen(dll), fl = strlen(func), need = dl + 1 + fl + 1;
    if (m->next_stub + WIN32_MISSING_STUB_SIZE > WIN32_MISSING_NAMES_VA ||
        m->next_name + need > WIN32_MISSING_END_VA)
        return WIN32_MISSING_STUB_VA;
    uint64_t name = m->next_name;
    memcpy((void *)name, dll, dl);
    *(char *)(name + dl) = '!';
    memcpy((void *)(name + dl + 1), func, fl);
    *(char *)(name + dl + 1 + fl) = '\0';
    m->next_name += need;
    uint64_t stub = m->next_stub;
    pe_emit_missing_stub(stub, name, m->idx);
    m->next_stub += WIN32_MISSING_STUB_SIZE;
    return stub;
}

/* W1: how many procaddr slots fit before the missing-import stubs. */
#define WIN32_PA_CAP ((WIN32_MISSING_STUB_VA - WIN32_PROCADDR_BASE_VA) / WIN32_PROCADDR_STRIDE)

/* W3: the fail entry (WIN32_FAIL_ENTRY_VA, from logs/w32/w3-tramp.S --
 * assembled offline with clang --target=x86_64-elf, bytes taken with
 * objcopy): the first instruction of a process whose loading failed; it calls
 * __toby_loadfail with the loader's failure text and status from the module
 * table (WIN32_MODTAB_BASE, 0x31008000, read at its fixed VA). */
static const uint8_t g_win32_fail_entry[] = {
    0x48, 0x83, 0xec, 0x28, 0x48, 0xb8, 0x00, 0x80, 0x00, 0x31, 0x00, 0x00,
    0x00, 0x00, 0x48, 0x8b, 0x48, 0x18, 0x8b, 0x50, 0x20, 0xff, 0x50, 0x08,
    0x0f, 0x0b,
};
/* _execute_onexit_table(table) (W3, same source file): call the functions in
 * [_first, _last) from the last to the first, skipping NULLs, then clear the
 * table's three pointers; returns 0. rbx/rsi/rdi are the x64 ABI's
 * callee-saved registers, so the walk survives each call. */
static const uint8_t g_win32_onexit_exec[] = {
    0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xcf, 0x48, 0x8b,
    0x1f, 0x48, 0x8b, 0x77, 0x08, 0x48, 0x39, 0xde, 0x76, 0x10, 0x48, 0x83,
    0xee, 0x08, 0x48, 0x8b, 0x06, 0x48, 0x85, 0xc0, 0x74, 0xef, 0xff, 0xd0,
    0xeb, 0xeb, 0x31, 0xc0, 0x48, 0x89, 0x07, 0x48, 0x89, 0x47, 0x08, 0x48,
    0x89, 0x47, 0x10, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0x5e, 0x5b, 0xc3,
};
/* W4, W5: the loader's CPL3 half (logs/w32/w5-ldr.S, which describes it):
 * LoadLibraryA/W/ExA/ExW, FreeLibrary, ExitThread, ExitProcess,
 * FreeLibraryAndExitThread, where CreateThread starts a thread, where a
 * process with DLLs or TLS callbacks starts, and (W7) the C runtime's exit,
 * which runs the .exe's atexit functions first -- each a slot of the jump table
 * at its start (pe.h's WIN32_LDR_*_VA) -- and the routine that makes the
 * calls a list from the kernel names. Each reaches the kernel through
 * __toby_ldr's thunk, read from the module table, so nothing is patched. */
static const uint8_t g_win32_ldr_stubs[] = {
    0xeb, 0x56, 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00, 0xeb, 0x56, 0x66, 0x0f,
    0x1f, 0x44, 0x00, 0x00, 0xeb, 0x59, 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00,
    0xeb, 0x56, 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00, 0xe9, 0xa9, 0x00, 0x00,
    0x00, 0x0f, 0x1f, 0x00, 0xe9, 0x46, 0x01, 0x00, 0x00, 0x0f, 0x1f, 0x00,
    0xe9, 0x7a, 0x01, 0x00, 0x00, 0x0f, 0x1f, 0x00, 0xe9, 0xdf, 0x00, 0x00,
    0x00, 0x0f, 0x1f, 0x00, 0xe9, 0xe6, 0x00, 0x00, 0x00, 0x0f, 0x1f, 0x00,
    0xe9, 0xc9, 0x01, 0x00, 0x00, 0x0f, 0x1f, 0x00, 0xe9, 0x93, 0x01, 0x00,
    0x00, 0x0f, 0x1f, 0x00, 0x45, 0x31, 0xc0, 0x45, 0x31, 0xc9, 0xeb, 0x16,
    0x45, 0x31, 0xc0, 0x41, 0xb9, 0x01, 0x00, 0x00, 0x00, 0xeb, 0x0b, 0x45,
    0x31, 0xc9, 0xeb, 0x06, 0x41, 0xb9, 0x01, 0x00, 0x00, 0x00, 0x53, 0x56,
    0x48, 0x81, 0xec, 0x38, 0x03, 0x00, 0x00, 0x48, 0x89, 0xca, 0x45, 0x89,
    0xc0, 0x49, 0xc1, 0xe1, 0x20, 0x4d, 0x09, 0xc8, 0x4c, 0x8d, 0x4c, 0x24,
    0x20, 0xb9, 0x01, 0x00, 0x00, 0x00, 0xe8, 0xbe, 0x01, 0x00, 0x00, 0x48,
    0x85, 0xc0, 0x74, 0x72, 0x48, 0x89, 0xc3, 0x48, 0x8d, 0x4c, 0x24, 0x20,
    0xe8, 0xb8, 0x01, 0x00, 0x00, 0x4c, 0x63, 0xc0, 0x48, 0x89, 0xda, 0x4c,
    0x8d, 0x4c, 0x24, 0x20, 0xb9, 0x02, 0x00, 0x00, 0x00, 0xe8, 0x97, 0x01,
    0x00, 0x00, 0x48, 0x85, 0xc0, 0x75, 0x4b, 0xbe, 0x5a, 0x04, 0x00, 0x00,
    0xeb, 0x26, 0x53, 0x56, 0x48, 0x81, 0xec, 0x38, 0x03, 0x00, 0x00, 0x48,
    0x89, 0xca, 0x45, 0x31, 0xc0, 0x4c, 0x8d, 0x4c, 0x24, 0x20, 0xb9, 0x03,
    0x00, 0x00, 0x00, 0xe8, 0x6d, 0x01, 0x00, 0x00, 0x48, 0x83, 0xf8, 0x02,
    0x75, 0x20, 0x31, 0xf6, 0x48, 0x8d, 0x4c, 0x24, 0x20, 0xe8, 0x67, 0x01,
    0x00, 0x00, 0x89, 0xf2, 0x45, 0x31, 0xc0, 0x4c, 0x8d, 0x4c, 0x24, 0x20,
    0xb9, 0x04, 0x00, 0x00, 0x00, 0xe8, 0x47, 0x01, 0x00, 0x00, 0x48, 0x81,
    0xc4, 0x38, 0x03, 0x00, 0x00, 0x5e, 0x5b, 0xc3, 0x48, 0x83, 0xec, 0x28,
    0x89, 0xd6, 0xe8, 0xa7, 0xff, 0xff, 0xff, 0x89, 0xf1, 0xeb, 0x48, 0x48,
    0x89, 0xfb, 0x48, 0x81, 0xec, 0x30, 0x03, 0x00, 0x00, 0xb9, 0x07, 0x00,
    0x00, 0x00, 0xba, 0x02, 0x00, 0x00, 0x00, 0x45, 0x31, 0xc0, 0x4c, 0x8d,
    0x4c, 0x24, 0x20, 0xe8, 0x0d, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x4c, 0x24,
    0x20, 0xe8, 0x0f, 0x01, 0x00, 0x00, 0xb9, 0x08, 0x00, 0x00, 0x00, 0xe8,
    0xf9, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x30, 0x03, 0x00, 0x00, 0x48,
    0x8b, 0x4b, 0x08, 0x48, 0x83, 0xec, 0x20, 0xff, 0x13, 0x89, 0xc1, 0x48,
    0x83, 0xe4, 0xf0, 0x48, 0x81, 0xec, 0x30, 0x03, 0x00, 0x00, 0x89, 0xce,
    0xb9, 0x07, 0x00, 0x00, 0x00, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0x89,
    0xf0, 0x4c, 0x8d, 0x4c, 0x24, 0x20, 0xe8, 0xc2, 0x00, 0x00, 0x00, 0x48,
    0x8d, 0x4c, 0x24, 0x20, 0xe8, 0xc4, 0x00, 0x00, 0x00, 0xb9, 0x0a, 0x00,
    0x00, 0x00, 0x89, 0xf2, 0xe8, 0xac, 0x00, 0x00, 0x00, 0x0f, 0x0b, 0x48,
    0x83, 0xe4, 0xf0, 0x48, 0x81, 0xec, 0x30, 0x03, 0x00, 0x00, 0x89, 0xce,
    0xb9, 0x09, 0x00, 0x00, 0x00, 0x89, 0xf2, 0x45, 0x31, 0xc0, 0x4c, 0x8d,
    0x4c, 0x24, 0x20, 0xe8, 0x89, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x4c, 0x24,
    0x20, 0xe8, 0x8b, 0x00, 0x00, 0x00, 0xb9, 0x0a, 0x00, 0x00, 0x00, 0x89,
    0xf2, 0xe8, 0x73, 0x00, 0x00, 0x00, 0x0f, 0x0b, 0x48, 0x83, 0xe4, 0xf0,
    0x48, 0x83, 0xec, 0x20, 0x89, 0xce, 0x48, 0xb8, 0x30, 0x80, 0x00, 0x31,
    0x00, 0x00, 0x00, 0x00, 0x31, 0xc9, 0x48, 0x87, 0x08, 0x48, 0x85, 0xc9,
    0x74, 0x0c, 0x48, 0xb8, 0xc0, 0x02, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xd0, 0x89, 0xf1, 0xeb, 0x99, 0x48, 0x81, 0xec, 0x38, 0x03, 0x00,
    0x00, 0xb9, 0x05, 0x00, 0x00, 0x00, 0x31, 0xd2, 0x45, 0x31, 0xc0, 0x4c,
    0x8d, 0x4c, 0x24, 0x20, 0xe8, 0x28, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x4c,
    0x24, 0x20, 0xe8, 0x2a, 0x00, 0x00, 0x00, 0x48, 0x63, 0xd0, 0xb9, 0x06,
    0x00, 0x00, 0x00, 0x45, 0x31, 0xc0, 0x4c, 0x8d, 0x4c, 0x24, 0x20, 0xe8,
    0x09, 0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x38, 0x03, 0x00, 0x00, 0xff,
    0xe0, 0x48, 0xb8, 0x28, 0x80, 0x00, 0x31, 0x00, 0x00, 0x00, 0x00, 0xff,
    0x20, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83,
    0xec, 0x28, 0x48, 0x89, 0xce, 0x48, 0x8b, 0x1e, 0x45, 0x31, 0xe4, 0x49,
    0x39, 0xdc, 0x73, 0x66, 0x4b, 0x8d, 0x04, 0x64, 0x48, 0x8d, 0x7c, 0xc6,
    0x08, 0x4c, 0x8b, 0x2f, 0x4d, 0x85, 0xed, 0x74, 0x2c, 0x4c, 0x8b, 0x77,
    0x10, 0x41, 0x0f, 0xba, 0xe6, 0x08, 0x72, 0x26, 0x48, 0x8b, 0x4f, 0x08,
    0x41, 0x0f, 0xb6, 0xd6, 0x45, 0x89, 0xf0, 0x41, 0xc1, 0xe8, 0x09, 0x41,
    0x83, 0xe0, 0x01, 0x41, 0xff, 0xd5, 0x41, 0x0f, 0xba, 0xe6, 0x0a, 0x73,
    0x04, 0x85, 0xc0, 0x74, 0x30, 0x49, 0xff, 0xc4, 0xeb, 0xb9, 0x49, 0x8b,
    0x45, 0x00, 0x48, 0x85, 0xc0, 0x74, 0xf2, 0x48, 0x8b, 0x4f, 0x08, 0x41,
    0x0f, 0xb6, 0xd6, 0x45, 0x89, 0xf0, 0x41, 0xc1, 0xe8, 0x09, 0x41, 0x83,
    0xe0, 0x01, 0xff, 0xd0, 0x49, 0x83, 0xc5, 0x08, 0xeb, 0xdc, 0xb8, 0xff,
    0xff, 0xff, 0xff, 0xeb, 0x03, 0x44, 0x89, 0xe0, 0x48, 0x83, 0xc4, 0x28,
    0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f, 0x5e, 0x5b, 0xc3,
};
#define WIN32_LDR_STUBS_OFF ((uint64_t)(WIN32_LDR_STUBS_VA - WIN32_SHIM_BASE))
_Static_assert(WIN32_MT_LDR == 0x28, "the loader stubs hard-code __toby_ldr's slot");
_Static_assert(WIN32_MT_ATEXIT == 0x30 && WIN32_ONEXIT_EXEC_VA == 0x300002C0ULL,
               "the CRT's exit stub hard-codes the atexit slot and _execute_onexit_table");
_Static_assert(WIN32_MT_ATEXIT + 8 <= WIN32_MT_KIUSER && WIN32_MT_KIUSER + 8 <= WIN32_MT_INIT_LIST,
               "the fault entry's slot sits between the atexit slot and the init list");
_Static_assert(8 + WIN32_LDR_BUF_MAX * 24 <= 0x310, "the loader stubs' buffer is 0x310 bytes");
_Static_assert(WIN32_LDR_CTL_TLS == 0x100 && WIN32_LDR_CTL_RES == 0x200 && WIN32_LDR_CTL_CHECK == 0x400,
               "the loader stubs hard-code the list's ctl bits");
#define WIN32_ONEXIT_EXEC_OFF ((uint64_t)(WIN32_ONEXIT_EXEC_VA - WIN32_SHIM_BASE))
#define WIN32_FAIL_ENTRY_OFF ((uint64_t)(WIN32_FAIL_ENTRY_VA - WIN32_SHIM_BASE))
_Static_assert(WIN32_MODTAB_BASE == 0x31008000ULL, "the fail entry hard-codes the module table's VA");
_Static_assert(WIN32_MT_LOADFAIL == 0x08 && WIN32_MT_FAIL_TEXT == 0x18 &&
               WIN32_MT_FAIL_STATUS == 0x20,
               "the fail entry hard-codes the module table's layout");

/* Lay down the marshalling gates, the user-mode trampolines and the procaddr
 * table (one thunk per shim) in the (already mapped, RW) shim area. None of it
 * depends on a module, so it runs once per process before any import is bound
 * (W3: split out of the old pe_resolve_imports). */
static void pe_setup_shim_area(void) {
    /* Lay down the marshalling gate at the top of the shim page and patch
     * in the dispatch syscall number. */
    uint8_t gate[WIN32_GATE_SIZE];
    memcpy(gate, g_win32_gate, WIN32_GATE_SIZE);
    uint32_t nr = ABI_SYS_WIN32_DISPATCH;
    memcpy(&gate[WIN32_GATE_IMM_OFF], &nr, 4);

    /* C20: the float-return gate, same dispatch number patched in. */
    uint8_t fgate[WIN32_FGATE_SIZE];
    memcpy(fgate, g_win32_fgate, WIN32_FGATE_SIZE);
    memcpy(&fgate[WIN32_FGATE_IMM_OFF], &nr, 4);

    /* DispatchMessage trampoline (C7), with the &wndproc CRT-data slot
     * patched into its abs64 load. */
    uint8_t disp[WIN32_DISPATCH_SIZE];
    memcpy(disp, g_win32_dispatch_stub, WIN32_DISPATCH_SIZE);
    uint64_t wndproc_slot = WIN32_CRT_DATA_BASE + WIN32_CRT_WNDPROC;
    memcpy(&disp[WIN32_DISPATCH_IMM_OFF], &wndproc_slot, 8);

    unsigned long uf = uaccess_begin();
    memcpy((void *)WIN32_SHIM_BASE, gate, WIN32_GATE_SIZE);
    memcpy((void *)(WIN32_SHIM_BASE + WIN32_DISPATCH_OFF), disp, WIN32_DISPATCH_SIZE);
    /* C14: the dialog modal-loop trampoline + its dedicated gate-thunk for the
     * __toby_dlgsvc shim (so the trampoline can reach the kernel at a fixed VA
     * with no load-time patching of the blob itself). */
    _Static_assert(WIN32_DLGTRAMP_OFF + sizeof(g_win32_dlg_tramp) <= WIN32_DLGSVC_THUNK_OFF,
                   "dialog trampoline overruns the dlgsvc thunk slot");
    _Static_assert(WIN32_DLGSVC_THUNK_OFF + WIN32_THUNK_SIZE <= WIN32_THUNK_BASE,
                   "dlgsvc thunk overruns the import-thunk region");
    memcpy((void *)(WIN32_SHIM_BASE + WIN32_DLGTRAMP_OFF), g_win32_dlg_tramp,
           WIN32_DLG_TRAMP_SIZE);
    int dlgsvc_idx = win32_shim_index("user32.dll", "__toby_dlgsvc");
    if (dlgsvc_idx >= 0)
        pe_emit_gate_thunk(WIN32_DLGSVC_THUNK_OFF, (uint32_t)dlgsvc_idx);
    /* C19b: the pure-CPL3 setjmp/longjmp leaf stubs (high in page 0). */
    _Static_assert(WIN32_SETJMP_OFF  + WIN32_SETJMP_STUB_SIZE  <= WIN32_LONGJMP_OFF,
                   "setjmp stub overruns longjmp stub");
    _Static_assert(WIN32_LONGJMP_OFF + WIN32_LONGJMP_STUB_SIZE <= 0x1000,
                   "longjmp stub overruns shim page 0");
    memcpy((void *)(WIN32_SHIM_BASE + WIN32_SETJMP_OFF),  g_win32_setjmp_stub,  WIN32_SETJMP_STUB_SIZE);
    memcpy((void *)(WIN32_SHIM_BASE + WIN32_LONGJMP_OFF), g_win32_longjmp_stub, WIN32_LONGJMP_STUB_SIZE);
    /* C20: the float-return gate, in the free gap between the dialog trampoline
     * and the dlgsvc thunk. */
    _Static_assert(WIN32_DLGTRAMP_OFF + sizeof(g_win32_dlg_tramp) <= WIN32_FGATE_OFF,
                   "float gate overlaps the dialog trampoline");
    _Static_assert(WIN32_FGATE_OFF + WIN32_FGATE_SIZE <= WIN32_DLGSVC_THUNK_OFF,
                   "float gate overruns the dlgsvc thunk slot");
    memcpy((void *)(WIN32_SHIM_BASE + WIN32_FGATE_OFF), fgate, WIN32_FGATE_SIZE);
    /* W3: the fail entry, in the area W1 freed; W4/W5: the loader's stubs. */
    _Static_assert(WIN32_FAIL_ENTRY_OFF >= WIN32_THUNK_BASE, "fail entry overlaps the dlgsvc thunk");
    _Static_assert(WIN32_FAIL_ENTRY_OFF + sizeof(g_win32_fail_entry) <= WIN32_ONEXIT_EXEC_OFF,
                   "fail entry overruns the onexit stub");
    _Static_assert(WIN32_ONEXIT_EXEC_OFF + sizeof(g_win32_onexit_exec) <= WIN32_LDR_STUBS_OFF,
                   "onexit stub overruns the loader stubs");
    _Static_assert(WIN32_LDR_STUBS_OFF + sizeof(g_win32_ldr_stubs) <= WIN32_SETJMP_OFF,
                   "the loader stubs overrun the setjmp stub");
    memcpy((void *)WIN32_FAIL_ENTRY_VA, g_win32_fail_entry, sizeof(g_win32_fail_entry));
    memcpy((void *)WIN32_ONEXIT_EXEC_VA, g_win32_onexit_exec, sizeof(g_win32_onexit_exec));
    memcpy((void *)WIN32_LDR_STUBS_VA, g_win32_ldr_stubs, sizeof(g_win32_ldr_stubs));

    /* C9: a thunk for EVERY shim, so GetProcAddress can return a callable
     * pointer for any shim resolved by name (slot i at WIN32_PROCADDR_BASE_VA
     * + i*WIN32_PROCADDR_STRIDE); since W1 every import of a shim binds to
     * these too. Bounded by the missing-import stubs. */
    int nshims = win32_shim_count();
    if ((uint64_t)nshims > WIN32_PA_CAP) {
        kprintf("[pe] WARNING: procaddr table (%d shims) overruns its %lu slots\n",
                nshims, (unsigned long)WIN32_PA_CAP);
        nshims = (int)WIN32_PA_CAP;
    }
    for (int i = 0; i < nshims; i++)
        pe_emit_gate_thunk(WIN32_PROCADDR_OFF + (uint64_t)i * WIN32_THUNK_SIZE,
                           (uint32_t)i);
    uaccess_end(uf);
}

/* ===== W3: DLLs from disk (docs/roadmap-2026-11.md) ========================
 * From Microsoft's PE format specification (the import directory, the export
 * directory with its name/ordinal/address tables and forwarder strings, base
 * relocations) and "Dynamic-link library search order". When an import names a
 * DLL tobyOS does not implement itself (win32_dll_is_builtin), the loader
 * finds the file, maps it at its preferred base -- or relocates it when that is
 * taken -- binds its own imports the same way (recursively), and resolves the
 * importer's IAT from its export table: by name (binary search of the sorted
 * name table), by ordinal, and through forwarders ("DLL.name" / "DLL.#n").
 * Search order, the desktop default with safe search: the .exe's directory,
 * the system directory (C:\Windows\System32), the Windows directory, the
 * current directory (PATH: not yet). A file name is matched exactly, then
 * case-insensitively, as Windows' file system would. Each DLL's entry point
 * is called at DLL_PROCESS_ATTACH (W5: by the process-start stub, after the
 * DLL's TLS callbacks), in dependency order (a DLL after every DLL it
 * imports), before the .exe's own TLS callbacks and entry. A DLL that
 * is not found, is not a PE32+ x64 image, or cannot be placed starts the
 * process at the fail entry: it says so on stderr and ends with the status
 * Windows uses (STATUS_DLL_NOT_FOUND, STATUS_INVALID_IMAGE_FORMAT). An import
 * a DLL does not export binds to a missing-import stub (W1), as for shims.
 * The record of what was mapped -- the module table -- lives in user memory,
 * as Windows' loader data does (in the PEB).
 *
 * W4: the same loader at run time (LoadLibrary/FreeLibrary/GetProcAddress/
 * GetModuleHandle/GetModuleFileName; the pe_rt_* functions at the end). A call
 * rebuilds the loader's state from the module table, does its work, and writes
 * the table back. LoadLibrary's DllMains, and FreeLibrary's, run in CPL3: the
 * stubs in g_win32_ldr_stubs call the kernel, then the entry points the kernel
 * lists, then the kernel again. A module keeps a reference count -- one per
 * LoadLibrary and one per module whose imports name it -- and FreeLibrary
 * unloads it when that reaches 0, with every module it imports that reaches 0
 * too, each after its DLL_PROCESS_DETACH; the modules loaded with the process
 * stay loaded (Windows' static dependencies stay too). Not yet: mapping as
 * data (LOAD_LIBRARY_AS_DATAFILE and its kin fail with ERROR_NOT_SUPPORTED).
 *
 * W5: a module's whole lifetime. Every call that changes the module table, or
 * runs DllMains, holds the loader lock (src/syscall.c keeps it, in the table's
 * header), so two threads loading at once no longer lose a record. Each
 * module's TLS callbacks are called with every reason its DllMain is, before
 * it: at DLL_PROCESS_ATTACH and DETACH, and -- on the thread itself, for
 * every loaded DLL that did not turn them off with DisableThreadLibraryCalls
 * -- at DLL_THREAD_ATTACH and DETACH; the .exe's TLS callbacks come after all
 * the DLLs'. ExitProcess (and the CRT's exit) ends the other threads, then
 * sends DLL_PROCESS_DETACH in the reverse of the order the DLLs were
 * initialized in (each record keeps its number). Every thread has its own
 * static-TLS block for every module with TLS: made with the thread
 * (src/syscall.c), or -- for a DLL loaded while it runs -- by LoadLibrary. The
 * orders are what Windows 11 does (programs/win-life30 runs there too). */

enum { PE_MOD_MAPPED, PE_MOD_BINDING, PE_MOD_DONE };

struct pe_mod {
    char     name[WIN32_MT_NAME_MAX];   /* lower-case file name, e.g. "dll28c.dll" */
    uint64_t base, size;       /* base 0: a free slot */
    uint64_t entry;            /* AddressOfEntryPoint as a VA, 0 if none */
    uint32_t export_rva, export_size;
    uint32_t import_rva, import_size;
    uint32_t tls_rva, tls_size;
    uint64_t name_va, path_va; /* the module table's copies */
    uint32_t flags;            /* WIN32_MTM_FLAG_* other than DLL (W4) */
    uint32_t refs;             /* W4: references (meaningless when pinned) */
    uint32_t tls_slot;         /* W4: its TLS slot, 0 if none */
    uint32_t init_seq;         /* W5: when it was initialized (1 first), 0 before */
    int      state;
    bool     is_dll;
    bool     fresh;            /* W4: mapped by this call -- unmapped if it fails */
};

struct pe_ctx {
    struct pe_mod mod[WIN32_MAX_MODULES];
    int      n;                         /* slots in use (the high-water mark) */
    int      init[WIN32_MAX_MODULES];   /* DLLs to initialize, in order */
    int      ninit;
    char     exe_dir[ABI_PATH_MAX];
    char     load_dir[ABI_PATH_MAX];    /* W4: LoadLibraryEx's DLL directory */
    const char *cwd;
    const char *dirs[4];                /* the search order */
    int      ndirs;
    uint64_t next_base;                 /* where a DLL goes when its base is taken */
    bool     failed;
    uint32_t fail_status;               /* W4: the first failure's NTSTATUS */
    bool     rt;                        /* W4: a run-time call (see pe_uread) */
    bool     no_load;                   /* W4: GetProcAddress: map nothing */
    bool     miss_open;                 /* W4: the missing-import pages are writable */
    bool     rt_absent;                 /* W7b: /lib/win32/tobyrt.dll is not there */
    struct pe_missing_area miss;
    uint32_t tls_used;                  /* bit n: TLS slot n is taken (0 is the .exe's) */
    uint64_t tls_next, tls_end;         /* next free byte / end of the mapped TLS area */
    uint32_t init_seq;                  /* W5: the last initialization number given */
    struct pe_load_info *out;           /* the .exe's TLS facts go here (process start) */
};

#define WIN32_TLS_SLOTS ((int)(WIN32_TLS_BLK_OFF / 8))
_Static_assert(WIN32_TLS_SLOTS <= 32, "tls_used is a 32-bit map");
_Static_assert(WIN32_MT_INIT_LIST + WIN32_MAX_MODULES * WIN32_MT_INIT_STRIDE <= WIN32_MT_MISS_STUB,
               "the init list overruns the loader state");
_Static_assert(WIN32_MT_TLS_USED + 4 <= WIN32_MT_LOCK_DEPTH && WIN32_MT_LOCK_DEPTH + 4 <= WIN32_MT_LOCK_OWNER &&
               WIN32_MT_LOCK_OWNER + 8 <= WIN32_MT_INIT_SEQ && WIN32_MT_INIT_SEQ + 4 <= WIN32_MT_MAIN_GONE &&
               WIN32_MT_MAIN_GONE + 4 <= WIN32_MT_EXITING && WIN32_MT_EXITING + 8 <= WIN32_MT_MODS,
               "the loader state overruns the records");
_Static_assert(WIN32_MT_MODS + WIN32_MAX_MODULES * WIN32_MT_MOD_STRIDE <= WIN32_MT_STRINGS,
               "the records overrun the strings");
_Static_assert(WIN32_MTM_TLS_SLOT + 4 <= WIN32_MTM_INIT_SEQ && WIN32_MTM_INIT_SEQ + 4 <= WIN32_MT_MOD_STRIDE,
               "a record overruns its stride");
_Static_assert(WIN32_MT_STRINGS + WIN32_MAX_MODULES * WIN32_MT_STR_STRIDE <= WIN32_MT_FAIL_AREA,
               "the strings overrun the failure text");
_Static_assert(WIN32_MT_FAIL_AREA < WIN32_MODTAB_BYTES, "the failure text is outside the table");

/* User memory the loader reads and writes. At process start it is the new
 * address space the loader has just mapped, while current_proc() is still the
 * PARENT (so copy_*_user's probes would walk the wrong page tables): plain
 * copies in a uaccess window, as in W3. At run time (W4) it is the program's
 * own memory -- the module table, the images -- which the program may have
 * changed or unmapped, so every access goes through copy_from_user /
 * copy_to_user, which check the pages first; a failed read gives zeros. */
static bool pe_uread(const struct pe_ctx *c, void *dst, uint64_t src, size_t n) {
    if (c->rt) {
        if (copy_from_user(dst, (const void *)(uintptr_t)src, n) == 0) return true;
        memset(dst, 0, n);
        return false;
    }
    unsigned long f = uaccess_begin();
    memcpy(dst, (const void *)src, n);
    uaccess_end(f);
    return true;
}
static bool pe_uwrite(const struct pe_ctx *c, uint64_t dst, const void *src, size_t n) {
    if (c->rt) return copy_to_user((void *)(uintptr_t)dst, src, n) == 0;
    unsigned long f = uaccess_begin();
    memcpy((void *)dst, src, n);
    uaccess_end(f);
    return true;
}
/* A NUL-terminated string at user `src`, which must end before `limit`.
 * Returns its length, or -1 if it does not fit `cap`, runs past `limit` or
 * cannot be read. Reads at most a page at a time. */
static int pe_ustr(const struct pe_ctx *c, char *dst, int cap, uint64_t src, uint64_t limit) {
    int n = 0;
    while (n < cap && src + (uint64_t)n < limit) {
        uint64_t a = src + (uint64_t)n;
        uint64_t chunk = PAGE_SIZE - (a & (PAGE_SIZE - 1));
        if (chunk > (uint64_t)(cap - n)) chunk = (uint64_t)(cap - n);
        if (chunk > limit - a) chunk = limit - a;
        if (!pe_uread(c, dst + n, a, chunk)) break;
        for (uint64_t i = 0; i < chunk; i++, n++)
            if (!dst[n]) return n;
    }
    dst[cap - 1] = '\0';
    return -1;
}
/* Page-table edits (vmm_map/vmm_protect/vmm_translate) act on the vmm's
 * EDITOR ROOT, which is per CPU and the kernel's by default. At process start
 * the spawn path has set it to the new address space for the whole load. At
 * run time (W4) each edit sets it to the calling process's for its own span
 * -- never across a file read, which can sleep and resume on another CPU. */
static uint64_t pe_root_enter(const struct pe_ctx *c) {
    if (!c->rt) return 0;
    struct proc *p = current_proc();
    return vmm_set_editor_root(p ? p->cr3 : 0);
}
static void pe_root_leave(const struct pe_ctx *c, uint64_t saved) {
    if (c->rt) (void)vmm_set_editor_root(saved);
}
static bool pe_rva_ok(const struct pe_mod *m, uint64_t rva, uint64_t len) {
    return rva < m->size && len <= m->size - rva;
}
static char pe_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool pe_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (pe_lower(*a) != pe_lower(*b)) return false;
    return *a == *b;
}

/* W4 (pe.h): the loader's key for a module name. Before W4 the loader kept
 * up to 59 characters of a longer name; a name that long now fails. */
int pe_module_name(const char *in, char *out, int cap) {
    const char *b = in;
    for (const char *q = in; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
    int n = 0;
    bool dot = false;
    for (; b[n]; n++) {
        if (n >= cap - 5) return -1;              /* room for ".dll" and the NUL */
        out[n] = pe_lower(b[n]);
        if (b[n] == '.') dot = true;
    }
    if (n == 0) return -1;
    if (out[n - 1] == '.') {                      /* "x." -- a name with no extension */
        if (--n == 0) return -1;
        out[n] = '\0';
        return n;
    }
    if (!dot) { memcpy(out + n, ".dll", 5); return n + 4; }
    out[n] = '\0';
    return n;
}

/* Slot mi's strings in the module table: fixed places (W4), so a slot freed
 * by FreeLibrary is reused without the string area growing. */
static uint64_t pe_slot_name_va(int mi) {
    return WIN32_MODTAB_BASE + WIN32_MT_STRINGS + (uint64_t)mi * WIN32_MT_STR_STRIDE;
}
static uint64_t pe_slot_path_va(int mi) { return pe_slot_name_va(mi) + WIN32_MT_NAME_MAX; }

/* Write module mi's name and Windows path to its slot. A path too long for the
 * slot is left out (GetModuleFileName then fails for that module). */
static void pe_slot_strings(struct pe_ctx *c, int mi, const char *winpath) {
    struct pe_mod *m = &c->mod[mi];
    m->name_va = pe_slot_name_va(mi);
    (void)pe_uwrite(c, m->name_va, m->name, strlen(m->name) + 1);
    m->path_va = 0;
    if (winpath && strlen(winpath) + 1 <= WIN32_MT_PATH_MAX) {
        m->path_va = pe_slot_path_va(mi);
        (void)pe_uwrite(c, m->path_va, winpath, strlen(winpath) + 1);
    }
}

/* Record the first load failure. At process start its text goes to the module
 * table, where the fail entry finds it; at run time LoadLibrary turns the
 * status into the calling thread's last error. */
static void pe_fail(struct pe_ctx *c, uint32_t status, const char *text) {
    if (c->failed) return;
    c->failed = true;
    c->fail_status = status;
    kprintf("[pe] load failure (0x%08x): %s\n", status, text);
    if (c->rt) return;
    uint64_t va = WIN32_MODTAB_BASE + WIN32_MT_FAIL_AREA;
    size_t n = strlen(text), room = WIN32_MODTAB_BYTES - WIN32_MT_FAIL_AREA - 1;
    if (n > room) n = room;
    char nul = '\0';
    (void)pe_uwrite(c, va, text, n);
    (void)pe_uwrite(c, va + n, &nul, 1);
    (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_FAIL_TEXT, &va, 8);
    (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_FAIL_STATUS, &status, 4);
}

/* The loaded module named `name` (a pe_module_name key), or -1. */
static int pe_loaded(const struct pe_ctx *c, const char *name) {
    for (int i = 0; i < c->n; i++)
        if (c->mod[i].base && strcmp(c->mod[i].name, name) == 0) return i;
    return -1;
}
/* The module loaded at `base`, or -1. */
static int pe_by_base(const struct pe_ctx *c, uint64_t base) {
    if (!base) return -1;
    for (int i = 0; i < c->n; i++)
        if (c->mod[i].base == base) return i;
    return -1;
}
/* The loaded module whose Windows path is `winpath` (without case), or -1. */
static int pe_loaded_path(const struct pe_ctx *c, const char *winpath) {
    char p[WIN32_MT_PATH_MAX];
    for (int i = 0; i < c->n; i++) {
        const struct pe_mod *m = &c->mod[i];
        if (!m->base || !m->path_va) continue;
        if (pe_ustr(c, p, sizeof p, m->path_va, m->path_va + WIN32_MT_PATH_MAX) > 0 &&
            pe_ieq(p, winpath))
            return i;
    }
    return -1;
}
/* A free slot for a new module, or -1 when all are taken. */
static int pe_slot_alloc(struct pe_ctx *c) {
    for (int i = 0; i < WIN32_MAX_MODULES; i++) {
        if (i < c->n && c->mod[i].base) continue;
        memset(&c->mod[i], 0, sizeof c->mod[i]);
        return i;
    }
    return -1;
}
/* A reference to module mi (W4): a LoadLibrary, or a module whose imports
 * name it. A module loaded with the process is pinned and not counted. */
static void pe_ref(struct pe_ctx *c, int mi) {
    if (!(c->mod[mi].flags & WIN32_MTM_FLAG_PINNED)) c->mod[mi].refs++;
}

/* ---- headers, placement, protection -------------------------------------- */

struct pe_hdr {
    const struct coff_header     *coff;
    const struct pe_opt_header64 *opt;
    const struct pe_data_dir     *dirs;
    const struct pe_section      *secs;
};

/* Validate a PE32+ x64 image in a kernel buffer and point h at its headers. */
static bool pe_parse(const void *image, size_t size, struct pe_hdr *h) {
    if (!pe_is_image(image, size)) return false;
    const struct dos_header *dos = (const struct dos_header *)image;
    if ((uint64_t)dos->e_lfanew + 4 + sizeof(struct coff_header) > size) return false;
    h->coff = (const struct coff_header *)((const uint8_t *)image + dos->e_lfanew + 4);
    if (h->coff->machine != PE_MACHINE_AMD64) return false;
    h->opt = (const struct pe_opt_header64 *)((const uint8_t *)h->coff + sizeof(*h->coff));
    if ((const uint8_t *)h->opt + sizeof(*h->opt) > (const uint8_t *)image + size) return false;
    if (h->opt->magic != PE_OPT_MAGIC_PE32P) return false;
    h->dirs = (const struct pe_data_dir *)((const uint8_t *)h->opt + sizeof(*h->opt));
    h->secs = (const struct pe_section *)((const uint8_t *)h->opt + h->coff->opt_header_size);
    if ((const uint8_t *)(h->secs + h->coff->num_sections) > (const uint8_t *)image + size)
        return false;
    if ((const uint8_t *)(h->dirs + h->opt->num_data_dirs) > (const uint8_t *)image + size)
        return false;
    return true;
}

static struct pe_data_dir pe_dir(const struct pe_hdr *h, int i) {
    struct pe_data_dir z = {0, 0};
    return (h->opt->num_data_dirs > (uint32_t)i) ? h->dirs[i] : z;
}

/* Copy headers + sections into the (mapped, RW) image span at `base` and apply
 * the base relocations for `delta` = base - preferred base. The span was just
 * mapped by this loader, at process start and at run time alike. */
static void pe_place(const void *image, size_t size, const struct pe_hdr *h,
                     uint64_t base, int64_t delta, uint64_t size_of_image) {
    unsigned long uf = uaccess_begin();
    uint32_t hdr = h->opt->size_of_headers;
    if (hdr > size) hdr = (uint32_t)size;
    if (hdr > size_of_image) hdr = (uint32_t)size_of_image;
    memcpy((void *)base, image, hdr);
    for (uint16_t i = 0; i < h->coff->num_sections; i++) {
        const struct pe_section *s = &h->secs[i];
        if ((uint64_t)s->virtual_addr >= size_of_image) continue;
        uint64_t room = size_of_image - s->virtual_addr;
        uint64_t va = base + s->virtual_addr;
        uint32_t fsz = s->raw_data_size;
        if (fsz && (uint64_t)s->raw_data_ptr + fsz <= size) {
            if (fsz > room) fsz = (uint32_t)room;
            memcpy((void *)va, (const uint8_t *)image + s->raw_data_ptr, fsz);
        } else {
            fsz = 0;
        }
        uint64_t vsz = s->virtual_size < room ? s->virtual_size : room;
        if (vsz > fsz) memset((void *)(va + fsz), 0, vsz - fsz);
    }
    uaccess_end(uf);

    struct pe_data_dir rd = pe_dir(h, PE_DIR_BASERELOC);
    if (delta == 0 || !rd.rva || !rd.size || (uint64_t)rd.rva + rd.size > size_of_image)
        return;
    uf = uaccess_begin();
    const uint8_t *rp  = (const uint8_t *)(base + rd.rva);
    const uint8_t *end = rp + rd.size;
    uint32_t nfix = 0, nother = 0;
    while (rp + sizeof(struct pe_base_reloc) <= end) {
        const struct pe_base_reloc *blk = (const struct pe_base_reloc *)rp;
        if (blk->block_size < sizeof(*blk) || rp + blk->block_size > end) break;
        uint32_t n = (blk->block_size - sizeof(*blk)) / 2;
        const uint16_t *e = (const uint16_t *)(rp + sizeof(*blk));
        for (uint32_t j = 0; j < n; j++) {
            uint32_t type = e[j] >> 12;
            uint64_t off = (uint64_t)blk->page_rva + (e[j] & 0xFFF);
            if (type == PE_RELOC_DIR64 && off + 8 <= size_of_image) {
                *(uint64_t *)(base + off) += (uint64_t)delta;
                nfix++;
            } else if (type != PE_RELOC_ABSOLUTE) {
                nother++;
            }
        }
        rp += blk->block_size;
    }
    uaccess_end(uf);
    kprintf("[pe] relocations applied at %p: %u DIR64 fixups (delta=0x%lx)%s\n",
            (void *)base, nfix, (unsigned long)delta,
            nother ? " -- and some of a type this loader does not apply" : "");
}

/* W^X for a mapped module: headers read-only, each section per its flags
 * (read from the module's own mapped headers). */
static void pe_protect(const struct pe_ctx *c, const struct pe_mod *m) {
    uint32_t lfanew;
    pe_uread(c, &lfanew, m->base + 0x3C, 4);
    if (!pe_rva_ok(m, lfanew, 4 + sizeof(struct coff_header))) return;
    struct coff_header coff;
    pe_uread(c, &coff, m->base + lfanew + 4, sizeof coff);
    uint64_t secs = (uint64_t)lfanew + 4 + sizeof coff + coff.opt_header_size;
    if (!pe_rva_ok(m, secs, (uint64_t)coff.num_sections * sizeof(struct pe_section))) return;
    /* (the reads below are of the module's own present pages: they cannot
     * sleep, so the editor root may stay set across the loop) */
    uint64_t root = pe_root_enter(c);
    (void)vmm_protect(m->base, PAGE_SIZE, VMM_PRESENT | VMM_USER | VMM_NX);
    for (uint16_t i = 0; i < coff.num_sections; i++) {
        struct pe_section s;
        pe_uread(c, &s, m->base + secs + (uint64_t)i * sizeof s, sizeof s);
        if (s.virtual_size == 0 || s.virtual_addr >= m->size) continue;
        uint64_t lo = m->base + (s.virtual_addr & ~(PAGE_SIZE - 1));
        uint64_t hi = m->base + round_up((uint64_t)s.virtual_addr + s.virtual_size, PAGE_SIZE);
        if (hi > m->base + m->size) hi = m->base + m->size;
        uint32_t f = VMM_PRESENT | VMM_USER;
        if (s.characteristics & PE_SCN_MEM_WRITE)     f |= VMM_WRITE;
        if (!(s.characteristics & PE_SCN_MEM_EXECUTE)) f |= VMM_NX;
        (void)vmm_protect(lo, hi - lo, f);
    }
    pe_root_leave(c, root);
}

/* ---- TLS (C18b, per module since W3) --------------------------------------
 * The pointer array at WIN32_TLS_BASE has one slot per module: the .exe's is
 * slot 0 (its _tls_index is 0, as before W3), each DLL with a TLS directory
 * gets a free slot (W4: FreeLibrary gives it back). The MAIN thread's blocks
 * are laid out after the array; every other thread has its own array and
 * blocks (W5: src/syscall.c makes them from pe_rt_tls_templates). */

/* Module m's TLS directory, when it has one that fits in the image. */
static bool pe_tls_dir(const struct pe_ctx *c, const struct pe_mod *m, struct pe_tls_dir64 *t) {
    if (!m->base || !m->tls_rva || m->tls_size < sizeof *t || !pe_rva_ok(m, m->tls_rva, sizeof *t))
        return false;
    return pe_uread(c, t, m->base + m->tls_rva, sizeof *t);
}

/* W5: the VA of module m's TLS callback array (NULL-terminated, in the image),
 * or 0 when it has none: no TLS directory, no array, or an empty one. The
 * stub walks the array itself, reading each entry when it calls it, as
 * Windows does. */
static uint64_t pe_tls_callbacks(const struct pe_ctx *c, const struct pe_mod *m) {
    struct pe_tls_dir64 t;
    if (!pe_tls_dir(c, m, &t) || !t.callbacks) return 0;
    if (t.callbacks < m->base || t.callbacks + 8 > m->base + m->size || (t.callbacks & 7)) return 0;
    uint64_t first = 0;
    if (!pe_uread(c, &first, t.callbacks, 8) || !first) return 0;
    return t.callbacks;
}
static bool pe_tls_map(struct pe_ctx *c, uint64_t end) {
    uint64_t need = round_up(end, PAGE_SIZE);
    if (need > WIN32_MODTAB_BASE) return false;
    if (need <= c->tls_end) return true;
    uint64_t root = pe_root_enter(c);
    bool ok = pe_map_user(c->tls_end, need);
    pe_root_leave(c, root);
    if (!ok) return false;
    unsigned long uf = uaccess_begin();
    memset((void *)c->tls_end, 0, need - c->tls_end);
    uaccess_end(uf);
    c->tls_end = need;
    return true;
}

static void pe_setup_tls(struct pe_ctx *c, int mi) {
    struct pe_mod *m = &c->mod[mi];
    if (!m->tls_rva || m->tls_size < sizeof(struct pe_tls_dir64) ||
        !pe_rva_ok(m, m->tls_rva, sizeof(struct pe_tls_dir64)))
        return;
    struct pe_tls_dir64 t;
    pe_uread(c, &t, m->base + m->tls_rva, sizeof t);
    uint64_t raw_size = (t.raw_end > t.raw_start) ? (t.raw_end - t.raw_start) : 0;
    uint64_t total = raw_size + (uint64_t)t.zero_fill;
    uint64_t img_end = m->base + m->size;
    if (t.raw_start < m->base || t.raw_start + raw_size > img_end || total > WIN32_TLS_MAX_BLK) {
        kprintf("[pe] .tls of %s: skipped raw=%p size=0x%lx total=0x%lx (range)\n",
                m->name, (void *)t.raw_start, (unsigned long)raw_size, (unsigned long)total);
        return;
    }
    int slot = 0;                                 /* the process's .exe: slot 0 */
    if (mi != 0) {
        for (slot = 1; slot < WIN32_TLS_SLOTS && (c->tls_used & (1u << slot)); slot++)
            ;
        if (slot >= WIN32_TLS_SLOTS) {
            kprintf("[pe] .tls of %s: no TLS slot left\n", m->name);
            return;
        }
    }
    total = round_up(total, 16);
    uint64_t blk = c->tls_next;
    if (!pe_tls_map(c, blk + (total ? total : 16))) {
        kprintf("[pe] .tls of %s: map failed\n", m->name);
        return;
    }
    if (total) {
        /* the module was just mapped and the block just made sure of */
        unsigned long uf = uaccess_begin();
        memcpy((void *)blk, (const void *)t.raw_start, raw_size);
        if (total > raw_size) memset((void *)(blk + raw_size), 0, total - raw_size);
        uaccess_end(uf);
        c->tls_next = blk + total;
    }
    c->tls_used |= 1u << slot;
    m->tls_slot = (uint32_t)slot;
    uint64_t slot_va = WIN32_TLS_BASE + (uint64_t)slot * 8;
    uint64_t blk_or_0 = total ? blk : 0;
    (void)pe_uwrite(c, slot_va, &blk_or_0, 8);
    uint32_t idx = (uint32_t)slot;
    if (t.index_addr >= m->base && t.index_addr + 4 <= img_end)
        (void)pe_uwrite(c, t.index_addr, &idx, 4);    /* _tls_index */
    uint64_t arr = WIN32_TLS_BASE;
    (void)pe_uwrite(c, WIN32_TEB_BASE + WIN32_TEB_TLSPTR, &arr, 8);
    if (mi == 0 && c->out) {
        c->out->tls_raw_va   = t.raw_start;
        c->out->tls_raw_size = raw_size;
        c->out->tls_total    = total;
        c->out->tls_index    = 0;
    }
    /* The mingw .tls callbacks (__dyn_tls_init/__dyn_tls_dtor) are NOT invoked:
     * they only service *dynamic* TLS initializers; every C1..C25 PE ships them
     * and runs correctly without them (C18b). */
    kprintf("[pe] .tls of %s: slot %d raw=%p size=0x%lx total=0x%lx blk=%p\n",
            m->name, slot, (void *)t.raw_start, (unsigned long)raw_size,
            (unsigned long)total, (void *)blk_or_0);
}

/* W4: give module m's TLS slot back (FreeLibrary, or a failed LoadLibrary) --
 * and its block, when it is the last one handed out (so a DLL loaded and freed
 * again and again reuses one block). Reads m's TLS directory: call it before
 * m is unmapped. */
static void pe_tls_release(struct pe_ctx *c, struct pe_mod *m) {
    if (!m->tls_slot || m->tls_slot >= (uint32_t)WIN32_TLS_SLOTS) return;
    uint64_t slot_va = WIN32_TLS_BASE + (uint64_t)m->tls_slot * 8;
    uint64_t blk = 0, zero = 0;
    pe_uread(c, &blk, slot_va, 8);
    (void)pe_uwrite(c, slot_va, &zero, 8);
    c->tls_used &= ~(1u << m->tls_slot);
    m->tls_slot = 0;
    if (!blk || !m->tls_rva || !pe_rva_ok(m, m->tls_rva, sizeof(struct pe_tls_dir64))) return;
    struct pe_tls_dir64 t;
    if (!pe_uread(c, &t, m->base + m->tls_rva, sizeof t)) return;
    uint64_t raw = (t.raw_end > t.raw_start) ? (t.raw_end - t.raw_start) : 0;
    uint64_t total = round_up(raw + (uint64_t)t.zero_fill, 16);
    if (blk >= WIN32_TLS_BASE + WIN32_TLS_BLK_OFF && blk + total == c->tls_next)
        c->tls_next = blk;
}

/* ---- import resolution ----------------------------------------------------- */

/* An import from a DLL tobyOS implements itself: a user-mode stub (calls back
 * into CPL3), the shim's procaddr thunk, or 0 when no shim has it. Shims are
 * named, so an ordinal import ("#n") is always 0. */
static uint64_t pe_builtin_import(const char *dll, const char *fname) {
    if (fname[0] == '#') return 0;
    uint64_t stub = win32_user_stub_va(dll, fname);
    if (stub) return stub;
    int idx = win32_shim_index(dll, fname);
    if (idx < 0 || (uint64_t)idx >= WIN32_PA_CAP) return 0;
    return WIN32_PROCADDR_BASE_VA + (uint64_t)idx * WIN32_PROCADDR_STRIDE;
}
/* W7b: the exception machinery runs in user mode, in tobyOS's own DLL
 * (programs/tobyrt: the dispatcher, the x64 unwinder, RtlUnwindEx,
 * __C_specific_handler, the top-level filter -- it calls the program's
 * handlers and walks its stack). An import of one of these names from any
 * built-in DLL (kernel32, ntdll, the CRT's) binds to that DLL's export of
 * the same name, the DLL loaded with the first such import and pinned. When
 * the file is not there they bind as before (shims, or missing-import stubs). */
#define PE_RT_KPATH "/lib/win32/tobyrt.dll"
#define PE_RT_NAME  "tobyrt.dll"
static const char *const g_pe_rt_names[] = {
    "RaiseException", "RtlRaiseException", "RtlCaptureContext", "RtlRestoreContext",
    "RtlLookupFunctionEntry", "RtlVirtualUnwind", "RtlUnwindEx", "RtlUnwind",
    "__C_specific_handler", "SetUnhandledExceptionFilter", "UnhandledExceptionFilter",
    "SetErrorMode", "GetErrorMode",
    /* W7c */
    "AddVectoredExceptionHandler", "RemoveVectoredExceptionHandler",
    "AddVectoredContinueHandler", "RemoveVectoredContinueHandler",
    "RtlAddVectoredExceptionHandler", "RtlRemoveVectoredExceptionHandler",
    "RtlAddVectoredContinueHandler", "RtlRemoveVectoredContinueHandler",
    /* W7d-2 */
    "GetCurrentThreadStackLimits",
    /* W7e */
    "RtlAddFunctionTable", "RtlDeleteFunctionTable", "RtlInstallFunctionTableCallback",
    "RtlAddGrowableFunctionTable", "RtlGrowFunctionTable", "RtlDeleteGrowableFunctionTable",
};
static bool pe_rt_name(const char *func) {
    for (unsigned i = 0; i < sizeof g_pe_rt_names / sizeof g_pe_rt_names[0]; i++)
        if (strcmp(func, g_pe_rt_names[i]) == 0) return true;
    return false;
}

/* W4 (pe.h): the same, for GetProcAddress on a built-in module -- so it
 * returns LoadLibraryA's CPL3 stub, not a shim that would skip DllMain.
 * W7b: a runtime name is the runtime's export, when the process has it. */
uint64_t win32_builtin_proc_va(const char *dll, const char *func) {
    if (!dll || !func) return 0;
    if (pe_rt_name(func)) {
        uint64_t base = pe_rt_find(PE_RT_NAME, 0);
        uint32_t err = 0;
        uint64_t va = base ? pe_rt_proc(base, func, 0, &err) : 0;
        if (va) return va;
    }
    return pe_builtin_import(dll, func);
}

static int pe_find_or_load(struct pe_ctx *c, const char *dll, const char *needed_by);

struct pe_export_dir {
    uint32_t characteristics, stamp;
    uint16_t major, minor;
    uint32_t name, base, nfuncs, nnames, funcs, names, ords;
};

/* `name` (or, when name is NULL, ordinal `ord`) in module mi's export table:
 * the VA it resolves to, following forwarders, or 0. */
static uint64_t pe_export_lookup(struct pe_ctx *c, int mi, const char *name,
                                 uint32_t ord, int depth) {
    const struct pe_mod *m = &c->mod[mi];
    struct pe_export_dir ed;
    if (depth > 8 || !m->export_rva || m->export_size < sizeof ed ||
        !pe_rva_ok(m, m->export_rva, sizeof ed))
        return 0;
    pe_uread(c, &ed, m->base + m->export_rva, sizeof ed);
    uint32_t idx;
    if (name) {
        if (!pe_rva_ok(m, ed.names, (uint64_t)ed.nnames * 4) ||
            !pe_rva_ok(m, ed.ords, (uint64_t)ed.nnames * 2))
            return 0;
        uint32_t lo = 0, hi = ed.nnames;
        bool found = false;
        uint32_t mid = 0;
        while (lo < hi) {                  /* the name table is sorted (spec) */
            mid = lo + (hi - lo) / 2;
            uint32_t nrva;
            pe_uread(c, &nrva, m->base + ed.names + (uint64_t)mid * 4, 4);
            char cand[256];
            if (!pe_rva_ok(m, nrva, 1) ||
                pe_ustr(c, cand, sizeof cand, m->base + nrva, m->base + m->size) < 0)
                return 0;
            int cmp = 0;
            for (int i = 0; ; i++) {
                unsigned char a = (unsigned char)name[i], b = (unsigned char)cand[i];
                if (a != b) { cmp = (a < b) ? -1 : 1; break; }
                if (!a) break;
            }
            if (cmp == 0) { found = true; break; }
            if (cmp < 0) hi = mid; else lo = mid + 1;
        }
        if (!found) return 0;
        uint16_t o;
        pe_uread(c, &o, m->base + ed.ords + (uint64_t)mid * 2, 2);
        idx = o;
    } else {
        if (ord < ed.base) return 0;
        idx = ord - ed.base;
    }
    if (idx >= ed.nfuncs || !pe_rva_ok(m, ed.funcs, (uint64_t)ed.nfuncs * 4)) return 0;
    uint32_t frva;
    pe_uread(c, &frva, m->base + ed.funcs + (uint64_t)idx * 4, 4);
    if (!frva || !pe_rva_ok(m, frva, 1)) return 0;
    if (frva >= m->export_rva && frva < m->export_rva + m->export_size) {
        /* A forwarder: "DLL.name" or "DLL.#ordinal". */
        char fw[160];
        if (pe_ustr(c, fw, sizeof fw, m->base + frva, m->base + m->size) < 0) return 0;
        int dot = -1;
        for (int i = 0; fw[i]; i++) if (fw[i] == '.') dot = i;
        if (dot <= 0 || !fw[dot + 1]) return 0;
        fw[dot] = '\0';
        char fdll[80];
        ksnprintf(fdll, sizeof fdll, "%s.dll", fw);
        const char *ffn = fw + dot + 1;
        kprintf("[pe]   %s forwards %s to %s!%s\n", m->name, name ? name : "(by ordinal)",
                fdll, ffn);
        if (win32_dll_is_builtin(fdll)) return pe_builtin_import(fdll, ffn);
        uint32_t ford = 0;
        if (ffn[0] == '#')
            for (const char *q = ffn + 1; *q >= '0' && *q <= '9'; q++) ford = ford * 10 + (uint32_t)(*q - '0');
        int fj = pe_find_or_load(c, fdll, m->name);
        if (fj < 0) return 0;
        /* W4: no import names a DLL reached this way, so nothing would ever
         * release it: it stays loaded. */
        if (c->rt) c->mod[fj].flags |= WIN32_MTM_FLAG_PINNED;
        return pe_export_lookup(c, fj, ffn[0] == '#' ? NULL : ffn, ford, depth + 1);
    }
    return m->base + frva;
}

static int pe_bind_module(struct pe_ctx *c, int mi);

/* Is [lo, hi) free for a module: unmapped and clear of the fixed Win32 pages? */
static bool pe_range_free(uint64_t lo, uint64_t hi) {
    if (lo < 0x10000ULL || hi > 0x0000800000000000ULL || hi <= lo) return false;
    if (lo < 0x32000000ULL && hi > 0x30000000ULL) return false;
    for (uint64_t v = lo; v < hi; v += PAGE_SIZE)
        if (vmm_translate(v) != 0) return false;
    return true;
}

/* Read `dir`/`name`, matching the name case-insensitively when the exact name
 * is not there. On success fills path and the image (borrowed or kmalloc'd). */
static bool pe_try_dir(const char *dir, const char *name, char *path, int cap,
                       void **img, size_t *sz, int *borrowed) {
    ksnprintf(path, (size_t)cap, "%s%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
    if (vfs_read_all_ref(path, img, sz, borrowed) == VFS_OK) return true;
    struct vfs_dir d;
    if (vfs_opendir(dir, &d) != VFS_OK) return false;
    struct vfs_dirent e;
    bool found = false;
    while (vfs_readdir(&d, &e) == VFS_OK) {
        if (pe_ieq(e.name, name)) {
            ksnprintf(path, (size_t)cap, "%s%s%s", dir,
                      (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", e.name);
            found = true;
            break;
        }
    }
    vfs_closedir(&d);
    return found && vfs_read_all_ref(path, img, sz, borrowed) == VFS_OK;
}

/* Map the image read from `path` (img/sz, borrowed or kmalloc'd -- consumed
 * here) as module `name`, then bind it. Returns its index, or -1 with the
 * failure recorded. */
static int pe_map_file(struct pe_ctx *c, const char *name, const char *path,
                       void *img, size_t sz, int borrowed, const char *needed_by) {
    char text[200];
    int mi = pe_slot_alloc(c);
    if (mi < 0) {
        if (!borrowed) kfree(img);
        ksnprintf(text, sizeof text, "%s cannot be loaded: more than %d modules (needed by %s)",
                  name, WIN32_MAX_MODULES, needed_by);
        pe_fail(c, WIN32_STATUS_NO_MEMORY, text);
        return -1;
    }
    struct pe_hdr h;
    uint64_t pref = 0, soi = 0, base = 0;
    bool ok = pe_parse(img, sz, &h);
    if (ok) {
        pref = h.opt->image_base;
        soi  = round_up(h.opt->size_of_image, PAGE_SIZE);
        ok = soi > 0 && soi < (1ULL << 32);
    }
    if (!ok) {
        if (!borrowed) kfree(img);
        ksnprintf(text, sizeof text, "%s is not a valid x64 image (needed by %s)", name, needed_by);
        pe_fail(c, WIN32_STATUS_INVALID_IMAGE_FORMAT, text);
        return -1;
    }
    struct pe_data_dir rd = pe_dir(&h, PE_DIR_BASERELOC);
    bool has_reloc = rd.rva && rd.size;
    uint64_t root = pe_root_enter(c);            /* placement: nothing here sleeps */
    base = pref;
    if (!pe_range_free(base, base + soi)) {
        base = 0;
        if (has_reloc) {
            for (uint64_t cand = c->next_base; cand + soi <= 0x00007F0000000000ULL; cand += 0x10000) {
                if (pe_range_free(cand, cand + soi)) { base = cand; break; }
            }
        }
        if (!base) {
            pe_root_leave(c, root);
            if (!borrowed) kfree(img);
            ksnprintf(text, sizeof text, "%s cannot be placed: its base %p is taken%s (needed by %s)",
                      name, (void *)pref, has_reloc ? "" : " and it has no relocations", needed_by);
            pe_fail(c, WIN32_STATUS_INVALID_IMAGE_FORMAT, text);
            return -1;
        }
        c->next_base = round_up(base + soi, 0x10000);
    }
    if (!pe_map_user(base, base + soi)) {
        pe_root_leave(c, root);
        if (!borrowed) kfree(img);
        /* a partial mapping is unmapped with the module (W4) or the process */
        if (c->rt) mmap_unmap_loader_pages(base, soi);
        ksnprintf(text, sizeof text, "%s: out of memory mapping it (needed by %s)", name, needed_by);
        pe_fail(c, WIN32_STATUS_NO_MEMORY, text);
        return -1;
    }
    pe_root_leave(c, root);
    pe_place(img, sz, &h, base, (int64_t)(base - pref), soi);

    struct pe_mod *m = &c->mod[mi];
    memcpy(m->name, name, strlen(name) + 1);
    m->base = base;
    m->size = soi;
    m->entry = h.opt->entry_point_rva ? base + h.opt->entry_point_rva : 0;
    struct pe_data_dir d;
    d = pe_dir(&h, PE_DIR_EXPORT); m->export_rva = d.rva; m->export_size = d.size;
    d = pe_dir(&h, PE_DIR_IMPORT); m->import_rva = d.rva; m->import_size = d.size;
    d = pe_dir(&h, PE_DIR_TLS);    m->tls_rva = d.rva;    m->tls_size = d.size;
    m->is_dll = (h.coff->characteristics & PE_FILE_DLL) != 0;
    m->state = PE_MOD_MAPPED;
    m->fresh = c->rt;
    /* Loaded with the process: pinned, and initialized by the process-start
     * stub before the .exe's entry (pe_rt_started marks it). At run time:
     * neither, yet. */
    m->flags = c->rt ? 0 : WIN32_MTM_FLAG_PINNED;
    if (mi >= c->n) c->n = mi + 1;
    char wp[ABI_PATH_MAX];
    pe_slot_strings(c, mi, win32_kpath_to_winpath(path, wp, sizeof wp) > 0 ? wp : 0);
    if (!borrowed) kfree(img);
    kprintf("[pe] DLL %s from %s at %p (preferred %p%s), needed by %s\n",
            name, path, (void *)base, (void *)pref,
            base == pref ? "" : ", relocated", needed_by);

    if (pe_bind_module(c, mi) < 0) return -1;
    return mi;
}

/* The module for DLL `dll`: already loaded, or found, mapped and bound now.
 * Returns its index, or -1 with the failure recorded. */
static int pe_find_or_load(struct pe_ctx *c, const char *dll, const char *needed_by) {
    if (c->failed) return -1;
    char name[WIN32_MT_NAME_MAX];
    char text[200];
    if (pe_module_name(dll, name, sizeof name) < 0) {
        ksnprintf(text, sizeof text, "%s: the name is too long (needed by %s)", dll, needed_by);
        pe_fail(c, WIN32_STATUS_DLL_NOT_FOUND, text);
        return -1;
    }
    int li = pe_loaded(c, name);
    if (li >= 0) return li;
    if (c->no_load) {
        ksnprintf(text, sizeof text, "%s is not loaded (needed by %s)", name, needed_by);
        pe_fail(c, WIN32_STATUS_DLL_NOT_FOUND, text);
        return -1;
    }
    char path[ABI_PATH_MAX];
    void *img = 0;
    size_t sz = 0;
    int borrowed = 0;
    bool got = false;
    for (int i = 0; i < c->ndirs && !got; i++)
        if (c->dirs[i] && c->dirs[i][0])
            got = pe_try_dir(c->dirs[i], name, path, sizeof path, &img, &sz, &borrowed);
    if (!got) {
        ksnprintf(text, sizeof text, "%s was not found (needed by %s)", name, needed_by);
        pe_fail(c, WIN32_STATUS_DLL_NOT_FOUND, text);
        return -1;
    }
    return pe_map_file(c, name, path, img, sz, borrowed, needed_by);
}

/* W7b: the runtime's module -- mapped and bound now if it is not yet, and
 * pinned (a process never unloads its exception machinery) -- or -1 when the
 * file is not there (or nothing may be mapped: GetProcAddress). */
static int pe_runtime(struct pe_ctx *c, const char *needed_by) {
    int li = pe_loaded(c, PE_RT_NAME);
    if (li >= 0) return li;
    if (c->rt_absent || c->no_load || c->failed) return -1;
    void *img = 0;
    size_t sz = 0;
    int borrowed = 0;
    if (vfs_read_all_ref(PE_RT_KPATH, &img, &sz, &borrowed) != VFS_OK) {
        kprintf("[pe] %s is not there: the exception functions bind as before W7b\n", PE_RT_KPATH);
        c->rt_absent = true;
        return -1;
    }
    int mi = pe_map_file(c, PE_RT_NAME, PE_RT_KPATH, img, sz, borrowed, needed_by);
    if (mi >= 0) c->mod[mi].flags |= WIN32_MTM_FLAG_PINNED;
    return mi;
}

/* An import a module's DLL does not export: bind it to a missing-import stub
 * (W1). At run time (W4) the stubs' pages are read-only and executable, so
 * they are opened for writing for the rest of the call (pe_rt_end closes
 * them). */
static uint64_t pe_missing_bind(struct pe_ctx *c, const char *dll, const char *fname) {
    if (c->rt && !c->miss_open) {
        uint64_t root = pe_root_enter(c);
        (void)vmm_protect(WIN32_MISSING_STUB_VA, WIN32_MISSING_END_VA - WIN32_MISSING_STUB_VA,
                          VMM_PRESENT | VMM_USER | VMM_WRITE);
        pe_root_leave(c, root);
        c->miss_open = true;
    }
    unsigned long uf = uaccess_begin();
    uint64_t va = pe_missing_stub(&c->miss, dll, fname);
    uaccess_end(uf);
    return va;
}

/* Bind module mi's imports (loading the DLLs they name), then set up its TLS,
 * tighten its pages, and queue its entry point for initialization -- after
 * every DLL it imports, since those were bound (and queued) inside the loop.
 * W4: at run time each import descriptor naming a DLL on disk is a reference
 * to that DLL (pe_rt_release drops the same ones). */
static int pe_bind_module(struct pe_ctx *c, int mi) {
    c->mod[mi].state = PE_MOD_BINDING;
    if (c->mod[mi].import_rva && pe_rva_ok(&c->mod[mi], c->mod[mi].import_rva, sizeof(struct pe_import_desc))) {
        for (uint64_t off = c->mod[mi].import_rva; ; off += sizeof(struct pe_import_desc)) {
            struct pe_mod *m = &c->mod[mi];      /* re-read: c->mod is stable, m is a view */
            if (!pe_rva_ok(m, off, sizeof(struct pe_import_desc))) break;
            struct pe_import_desc desc;
            pe_uread(c, &desc, m->base + off, sizeof desc);
            if (!desc.name_rva) break;
            char dll[80];
            if (!pe_rva_ok(m, desc.name_rva, 1) ||
                pe_ustr(c, dll, sizeof dll, m->base + desc.name_rva, m->base + m->size) < 0)
                break;
            bool builtin = win32_dll_is_builtin(dll);
            int dj = -1;
            if (!builtin) {
                dj = pe_find_or_load(c, dll, m->name);
                if (dj < 0) return -1;
                if (c->rt) pe_ref(c, dj);
            }
            uint64_t iat   = m->base + desc.first_thunk;
            uint64_t names = m->base + (desc.original_first_thunk ? desc.original_first_thunk
                                                                  : desc.first_thunk);
            for (uint64_t k = 0; ; k++) {
                if (!pe_rva_ok(m, (names - m->base) + k * 8, 8) ||
                    !pe_rva_ok(m, (iat - m->base) + k * 8, 8))
                    break;
                uint64_t ent;
                pe_uread(c, &ent, names + k * 8, 8);
                if (!ent) break;
                char fname[128];
                bool by_ord = (ent & PE_ORDINAL_FLAG) != 0;
                if (by_ord) {
                    ksnprintf(fname, sizeof fname, "#%u", (unsigned)(ent & 0xFFFFu));
                } else if (!pe_rva_ok(m, (uint32_t)ent + 2, 1) ||
                           pe_ustr(c, fname, sizeof fname, m->base + (uint32_t)ent + 2,
                                   m->base + m->size) < 0) {
                    ksnprintf(fname, sizeof fname, "(bad name)");
                }
                uint64_t va = 0;
                const char *to = builtin ? "shim" : c->mod[dj].name;
                if (builtin && !by_ord && pe_rt_name(fname)) {
                    int rj = pe_runtime(c, c->mod[mi].name);
                    if (c->failed) return -1;
                    if (rj >= 0 && (va = pe_export_lookup(c, rj, fname, 0, 0)) != 0)
                        to = PE_RT_NAME;
                }
                if (!va)
                    va = builtin
                        ? pe_builtin_import(dll, fname)
                        : pe_export_lookup(c, dj, by_ord ? NULL : fname,
                                           by_ord ? (uint32_t)(ent & 0xFFFFu) : 0, 0);
                if (c->failed) return -1;          /* a forwarder's DLL failed */
                if (va) {
                    kprintf("[pe]   bind %s!%s -> %s@%p\n", dll, fname, to, (void *)va);
                } else {
                    va = pe_missing_bind(c, dll, fname);
                    kprintf("[pe] unresolved import %s!%s -> missing-stub@%p\n",
                            dll, fname, (void *)va);
                }
                (void)pe_uwrite(c, iat + k * 8, &va, 8);
            }
        }
    }
    struct pe_mod *m = &c->mod[mi];
    pe_setup_tls(c, mi);
    pe_protect(c, m);
    m->state = PE_MOD_DONE;
    if (m->is_dll && c->ninit < WIN32_MAX_MODULES)     /* W5: entry or not */
        c->init[c->ninit++] = mi;
    return 0;
}

/* The module table's records and the loader's cursors, from the loader's
 * state: every slot, a free one as zeros. */
static void pe_commit(struct pe_ctx *c) {
    uint32_t nmod = (uint32_t)c->n;
    (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_MOD_COUNT, &nmod, 4);
    uint64_t st[4] = { c->miss.next_stub, c->miss.next_name, c->tls_next, c->tls_end };
    (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_MISS_STUB, st, sizeof st);
    (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_TLS_USED, &c->tls_used, 4);
    (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_INIT_SEQ, &c->init_seq, 4);
    for (int i = 0; i < WIN32_MAX_MODULES; i++) {
        const struct pe_mod *m = &c->mod[i];
        uint8_t r[WIN32_MT_MOD_STRIDE];
        memset(r, 0, sizeof r);
        if (i < c->n && m->base) {
            uint32_t flags = m->flags | (m->is_dll ? WIN32_MTM_FLAG_DLL : 0);
            memcpy(r + WIN32_MTM_BASE, &m->base, 8);
            memcpy(r + WIN32_MTM_SIZE, &m->size, 8);
            memcpy(r + WIN32_MTM_ENTRY, &m->entry, 8);
            memcpy(r + WIN32_MTM_NAME, &m->name_va, 8);
            memcpy(r + WIN32_MTM_PATH, &m->path_va, 8);
            memcpy(r + WIN32_MTM_EXPORT_RVA, &m->export_rva, 4);
            memcpy(r + WIN32_MTM_EXPORT_SIZE, &m->export_size, 4);
            memcpy(r + WIN32_MTM_FLAGS, &flags, 4);
            memcpy(r + WIN32_MTM_REFS, &m->refs, 4);
            memcpy(r + WIN32_MTM_TLS_SLOT, &m->tls_slot, 4);
            memcpy(r + WIN32_MTM_INIT_SEQ, &m->init_seq, 4);
        }
        (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_MODS + (uint64_t)i * WIN32_MT_MOD_STRIDE, r, sizeof r);
    }
    /* W7d: where the kernel sends a faulting thread (src/win32_seh.c) -- the
     * runtime's entry while the runtime is mapped, else 0 (the fault takes
     * the old path). Written with the committed records, so it never names
     * a runtime a failed LoadLibrary has unmapped again. */
    int rj = pe_loaded(c, PE_RT_NAME);
    uint64_t ki = rj >= 0 ? pe_export_lookup(c, rj, "KiUserExceptionDispatcher", 0, 0) : 0;
    (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_KIUSER, &ki, 8);
}

/* At process start: the header, the init list and the records. */
static void pe_write_modtab(struct pe_ctx *c, uint64_t exe_entry) {
    uint32_t ninit = (uint32_t)c->ninit;
    (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_EXE_ENTRY, &exe_entry, 8);
    (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_INIT_COUNT, &ninit, 4);
    for (int i = 0; i < c->ninit; i++) {
        const struct pe_mod *m = &c->mod[c->init[i]];
        uint64_t e[3] = { m->entry, m->base, m->name_va };
        (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_INIT_LIST + (uint64_t)i * WIN32_MT_INIT_STRIDE, e, sizeof e);
    }
    pe_commit(c);
}

/* ===== W4: the run-time loader ============================================== */

/* LoadLibraryEx's dwFlags (Microsoft's LoadLibraryExW documentation). */
#define LLF_DONT_RESOLVE_DLL_REFERENCES    0x00000001u
#define LLF_AS_DATAFILE                    0x00000002u
#define LLF_WITH_ALTERED_SEARCH_PATH       0x00000008u
#define LLF_AS_IMAGE_RESOURCE              0x00000020u
#define LLF_AS_DATAFILE_EXCLUSIVE          0x00000040u
#define LLF_SEARCH_DLL_LOAD_DIR            0x00000100u
#define LLF_SEARCH_APPLICATION_DIR         0x00000200u
#define LLF_SEARCH_USER_DIRS               0x00000400u
#define LLF_SEARCH_SYSTEM32                0x00000800u
#define LLF_SEARCH_DEFAULT_DIRS            0x00001000u
#define LLF_SEARCH_MASK                    0x00001F00u
#define LLF_AS_DATA (LLF_DONT_RESOLVE_DLL_REFERENCES | LLF_AS_DATAFILE | \
                     LLF_AS_IMAGE_RESOURCE | LLF_AS_DATAFILE_EXCLUSIVE)

#define W32E_NOT_ENOUGH_MEMORY  8u
#define W32E_INVALID_DATA       13u
#define W32E_NOT_SUPPORTED      50u
#define W32E_INVALID_PARAMETER  87u
#define W32E_MOD_NOT_FOUND      126u
#define W32E_PROC_NOT_FOUND     127u
#define W32E_BAD_EXE_FORMAT     193u

/* The module table's slot records name in-range user memory? (They are the
 * program's to change: a record that is not is refused, not followed.) */
static bool pe_rt_range_ok(uint64_t base, uint64_t size) {
    if ((base | size) & (PAGE_SIZE - 1)) return false;
    if (size == 0 || size >= (1ULL << 32)) return false;
    if (base < 0x10000ULL || base + size > 0x0000800000000000ULL) return false;
    if (base < 0x32000000ULL && base + size > 0x30000000ULL) return false;
    return true;
}

/* Module m's import and TLS directories, from its mapped headers. */
static void pe_rt_dirs_of(struct pe_ctx *c, struct pe_mod *m) {
    uint32_t lfanew = 0;
    pe_uread(c, &lfanew, m->base + 0x3C, 4);
    struct coff_header coff;
    struct pe_opt_header64 opt;
    uint64_t at = (uint64_t)lfanew + 4;
    if (!pe_rva_ok(m, at, sizeof coff + sizeof opt)) return;
    pe_uread(c, &coff, m->base + at, sizeof coff);
    pe_uread(c, &opt, m->base + at + sizeof coff, sizeof opt);
    struct pe_data_dir d[PE_DIR_TLS + 1];
    memset(d, 0, sizeof d);
    uint32_t nd = opt.num_data_dirs < (uint32_t)(PE_DIR_TLS + 1) ? opt.num_data_dirs
                                                                 : (uint32_t)(PE_DIR_TLS + 1);
    uint64_t dirs = at + sizeof coff + sizeof opt;
    if (nd && pe_rva_ok(m, dirs, (uint64_t)nd * sizeof d[0]))
        pe_uread(c, d, m->base + dirs, (size_t)nd * sizeof d[0]);
    m->import_rva = d[PE_DIR_IMPORT].rva; m->import_size = d[PE_DIR_IMPORT].size;
    m->tls_rva    = d[PE_DIR_TLS].rva;    m->tls_size    = d[PE_DIR_TLS].size;
}

/* A run-time call's loader state, rebuilt from the current process's module
 * table. NULL, with *err set, when the process has none (or it does not hold
 * together), or there is no memory. */
static struct pe_ctx *pe_rt_begin(uint32_t *err) {
    struct proc *p = current_proc();
    struct proc *lead = (p && p->is_thread) ? proc_lookup(p->tgid) : p;
    *err = W32E_INVALID_DATA;
    if (!p || !lead || !lead->win_image_base) return 0;
    struct pe_ctx *c = kmalloc(sizeof *c);
    if (!c) { *err = W32E_NOT_ENOUGH_MEMORY; return 0; }
    memset(c, 0, sizeof *c);
    c->rt = true;

    uint32_t nmod = 0, used = 0;
    uint64_t st[4];
    if (!pe_uread(c, &nmod, WIN32_MODTAB_BASE + WIN32_MT_MOD_COUNT, 4) ||
        !pe_uread(c, st, WIN32_MODTAB_BASE + WIN32_MT_MISS_STUB, sizeof st) ||
        !pe_uread(c, &used, WIN32_MODTAB_BASE + WIN32_MT_TLS_USED, 4) ||
        !pe_uread(c, &c->init_seq, WIN32_MODTAB_BASE + WIN32_MT_INIT_SEQ, 4) ||
        nmod == 0 || nmod > WIN32_MAX_MODULES)
        goto bad;
    c->n = (int)nmod;

    /* the cursors: any that does not hold together reads as "full" */
    c->miss.idx = win32_shim_index("kernel32.dll", "__toby_missing");
    c->miss.next_stub = st[0];
    c->miss.next_name = st[1];
    if (st[0] < WIN32_MISSING_STUB_VA || st[0] > WIN32_MISSING_NAMES_VA ||
        (st[0] - WIN32_MISSING_STUB_VA) % WIN32_MISSING_STUB_SIZE != 0 ||
        st[1] < WIN32_MISSING_NAMES_VA || st[1] > WIN32_MISSING_END_VA) {
        c->miss.next_stub = WIN32_MISSING_NAMES_VA;
        c->miss.next_name = WIN32_MISSING_END_VA;
    }
    c->tls_next = st[2];
    c->tls_end  = st[3];
    if ((c->tls_end & (PAGE_SIZE - 1)) || c->tls_end < WIN32_TLS_BASE ||
        c->tls_end > WIN32_MODTAB_BASE || c->tls_next < WIN32_TLS_BASE + WIN32_TLS_BLK_OFF ||
        c->tls_next > WIN32_MODTAB_BASE) {
        c->tls_end = WIN32_MODTAB_BASE;
        c->tls_next = WIN32_MODTAB_BASE;
    }
    c->tls_used = used | 1u;                      /* slot 0 is the .exe's */

    for (int i = 0; i < c->n; i++) {
        uint8_t r[WIN32_MT_MOD_STRIDE];
        if (!pe_uread(c, r, WIN32_MODTAB_BASE + WIN32_MT_MODS + (uint64_t)i * WIN32_MT_MOD_STRIDE, sizeof r))
            goto bad;
        struct pe_mod *m = &c->mod[i];
        uint64_t path_va;
        uint32_t flags;
        memcpy(&m->base, r + WIN32_MTM_BASE, 8);
        if (!m->base) continue;                   /* a free slot */
        memcpy(&m->size, r + WIN32_MTM_SIZE, 8);
        memcpy(&m->entry, r + WIN32_MTM_ENTRY, 8);
        memcpy(&path_va, r + WIN32_MTM_PATH, 8);
        memcpy(&m->export_rva, r + WIN32_MTM_EXPORT_RVA, 4);
        memcpy(&m->export_size, r + WIN32_MTM_EXPORT_SIZE, 4);
        memcpy(&flags, r + WIN32_MTM_FLAGS, 4);
        memcpy(&m->refs, r + WIN32_MTM_REFS, 4);
        memcpy(&m->tls_slot, r + WIN32_MTM_TLS_SLOT, 4);
        memcpy(&m->init_seq, r + WIN32_MTM_INIT_SEQ, 4);
        if (!pe_rt_range_ok(m->base, m->size) || (i == 0 && (flags & WIN32_MTM_FLAG_DLL)) ||
            m->tls_slot >= (uint32_t)WIN32_TLS_SLOTS)
            goto bad;
        m->is_dll  = (flags & WIN32_MTM_FLAG_DLL) != 0;
        m->flags   = flags & ~WIN32_MTM_FLAG_DLL;
        if (i == 0) m->flags |= WIN32_MTM_FLAG_PINNED;
        m->state   = PE_MOD_DONE;
        m->name_va = pe_slot_name_va(i);
        m->path_va = path_va ? pe_slot_path_va(i) : 0;
        if (pe_ustr(c, m->name, sizeof m->name, m->name_va, m->name_va + WIN32_MT_NAME_MAX) <= 0)
            goto bad;
        for (char *q = m->name; *q; q++) *q = pe_lower(*q);
        pe_rt_dirs_of(c, m);
    }

    /* the search order's places: the .exe's directory and the current one */
    {
        const char *ep = lead->exe_path[0] ? lead->exe_path : "/";
        int last = 0;
        for (int i = 0; ep[i]; i++) if (ep[i] == '/') last = i;
        int dn = last > 0 ? last : 1;
        if (dn >= (int)sizeof c->exe_dir) dn = (int)sizeof c->exe_dir - 1;
        memcpy(c->exe_dir, ep, (size_t)dn);
        c->exe_dir[dn] = '\0';
    }
    c->cwd = p->cwd[0] ? p->cwd : "/";
    c->next_base = 0x0000000200000000ULL;
    *err = 0;
    return c;
bad:
    kprintf("[pe] the module table of pid %d does not hold together -- refusing the call\n", p->pid);
    kfree(c);
    return 0;
}

/* End a run-time call: close the missing-import pages again, free the state. */
static void pe_rt_end(struct pe_ctx *c) {
    if (c->miss_open) {
        uint64_t root = pe_root_enter(c);
        (void)vmm_protect(WIN32_MISSING_STUB_VA, WIN32_MISSING_END_VA - WIN32_MISSING_STUB_VA,
                          VMM_PRESENT | VMM_USER);
        pe_root_leave(c, root);
    }
    kfree(c);
}

/* A failed LoadLibrary: unmap every module this call mapped. Their records were
 * never written, and the table's cursors are left as they were (what the call
 * took from the missing-import and TLS areas is simply taken again). */
static void pe_rt_rollback(struct pe_ctx *c) {
    for (int i = 0; i < c->n; i++) {
        struct pe_mod *m = &c->mod[i];
        if (!m->base || !m->fresh) continue;
        pe_tls_release(c, m);
        mmap_unmap_loader_pages(m->base, m->size);
        kprintf("[pe] LoadLibrary failed: unmapped %s at %p\n", m->name, (void *)m->base);
        memset(m, 0, sizeof *m);
    }
}

/* Drop a reference to module mi. When none is left it joins `list` (to be
 * detached and unmapped), and so does every module it imports that has none
 * left after it -- a module after all its importers, the order Windows
 * detaches in. A pinned module, or one already listed, is left alone. */
static void pe_rt_release(struct pe_ctx *c, int mi, int *list, int *nl) {
    struct pe_mod *m = &c->mod[mi];
    if (!m->base || (m->flags & (WIN32_MTM_FLAG_PINNED | WIN32_MTM_FLAG_UNLOAD))) return;
    if (m->refs > 0) m->refs--;
    if (m->refs > 0) return;
    m->flags |= WIN32_MTM_FLAG_UNLOAD;
    if (*nl < WIN32_MAX_MODULES) list[(*nl)++] = mi;
    if (!m->import_rva) return;
    for (uint64_t k = 0; k < 512; k++) {
        uint64_t off = m->import_rva + k * sizeof(struct pe_import_desc);
        if (!pe_rva_ok(m, off, sizeof(struct pe_import_desc))) break;
        struct pe_import_desc desc;
        if (!pe_uread(c, &desc, m->base + off, sizeof desc) || !desc.name_rva) break;
        char dll[80], key[WIN32_MT_NAME_MAX];
        if (!pe_rva_ok(m, desc.name_rva, 1) ||
            pe_ustr(c, dll, sizeof dll, m->base + desc.name_rva, m->base + m->size) < 0)
            break;
        if (win32_dll_is_builtin(dll) || pe_module_name(dll, key, sizeof key) < 0) continue;
        int j = pe_loaded(c, key);
        if (j >= 0 && j != mi) pe_rt_release(c, j, list, nl);
    }
}

/* A list for a stub (pe.h): b[0] the count, then {fn, base, ctl} triples. */
#define PE_LIST_WORDS (1 + 3 * WIN32_LDR_BUF_MAX)
#define PE_DLL_PROCESS_DETACH 0u
#define PE_DLL_PROCESS_ATTACH 1u
#define PE_DLL_THREAD_ATTACH  2u
#define PE_DLL_THREAD_DETACH  3u

static void pe_list_add(uint64_t *b, uint64_t fn, uint64_t base, uint32_t ctl) {
    uint64_t n = b[0];
    if (n >= WIN32_LDR_BUF_MAX) return;
    b[1 + 3 * n] = fn;
    b[2 + 3 * n] = base;
    b[3 + 3 * n] = ctl;
    b[0] = n + 1;
}

/* W5: module m's calls for one notification: its TLS callbacks, then (a DLL,
 * `dllmain`) its entry point; ctl = the reason and flags, WIN32_LDR_CTL_CHECK
 * for the entry point only (a TLS callback returns nothing). A module with
 * nothing to call is listed with fn 0 when `always`, so the kernel half of a
 * stub still finds it by its base afterwards. */
static void pe_note(const struct pe_ctx *c, const struct pe_mod *m, uint64_t *b,
                    uint32_t ctl, bool dllmain, bool always) {
    uint64_t before = b[0];
    uint64_t cb = pe_tls_callbacks(c, m);
    if (cb) pe_list_add(b, cb, m->base, (ctl & ~WIN32_LDR_CTL_CHECK) | WIN32_LDR_CTL_TLS);
    if (dllmain && m->is_dll && m->entry) pe_list_add(b, m->entry, m->base, ctl);
    if (b[0] == before && always) pe_list_add(b, 0, m->base, 0);
}

static bool pe_list_put(uint64_t ubuf, const uint64_t *b) {
    return copy_to_user((void *)(uintptr_t)ubuf, b, 8 + (size_t)b[0] * 24) == 0;
}

/* ubuf <- `list`'s DLL_PROCESS_DETACH calls (lpReserved NULL: FreeLibrary, or
 * a failed LoadLibrary): a module that was initialized gets them, one that
 * was not is listed with nothing to call (unmapped without a call). */
static bool pe_rt_put_list(struct pe_ctx *c, uint64_t ubuf, const int *list, int nl) {
    uint64_t b[PE_LIST_WORDS];
    b[0] = 0;
    for (int k = 0; k < nl; k++) {
        const struct pe_mod *m = &c->mod[list[k]];
        if (m->flags & WIN32_MTM_FLAG_INITED) pe_note(c, m, b, PE_DLL_PROCESS_DETACH, true, true);
        else pe_list_add(b, 0, m->base, 0);
    }
    return pe_list_put(ubuf, b);
}
/* The list at ubuf (count clamped to the buffer). */
static int pe_rt_get_list(uint64_t ubuf, uint64_t *b) {
    if (copy_from_user(b, (const void *)(uintptr_t)ubuf, 8) != 0) return 0;
    int n = b[0] > WIN32_LDR_BUF_MAX ? 0 : (int)b[0];
    if (n && copy_from_user(b + 1, (const void *)(uintptr_t)(ubuf + 8), (size_t)n * 24) != 0) return 0;
    return n;
}

static uint32_t pe_rt_status_err(uint32_t status) {
    switch (status) {
    case WIN32_STATUS_INVALID_IMAGE_FORMAT: return W32E_BAD_EXE_FORMAT;
    case WIN32_STATUS_NO_MEMORY:            return W32E_NOT_ENOUGH_MEMORY;
    default:                                return W32E_MOD_NOT_FOUND;
    }
}

/* Split a '/'-path into its directory (dir, "/" for the root) and file. */
static const char *pe_split(const char *path, char *dir, int cap) {
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    int dn = last > 0 ? last : (last == 0 ? 1 : 0);
    if (dn >= cap) dn = cap - 1;
    memcpy(dir, path, (size_t)dn);
    dir[dn] = '\0';
    return path + last + 1;
}

uint64_t pe_rt_load(const struct pe_rt_req *rq, uint64_t ubuf, uint32_t *err) {
    uint64_t zero = 0;
    *err = 0;
    if (copy_to_user((void *)(uintptr_t)ubuf, &zero, 8) != 0) { *err = W32E_INVALID_PARAMETER; return 0; }
    uint32_t fl = rq->flags;
    const char *spec = rq->kpath ? rq->kpath : rq->name;
    if (!spec || !spec[0]) { *err = W32E_INVALID_PARAMETER; return 0; }
    if (fl & LLF_AS_DATA) {
        kprintf("[pe] LoadLibraryEx(%s, 0x%x): mapping as data is not implemented\n", spec, fl);
        *err = W32E_NOT_SUPPORTED;
        return 0;
    }
    /* documented: the altered order cannot be combined with the SEARCH flags,
     * and DLL_LOAD_DIR needs a fully qualified path */
    if (((fl & LLF_WITH_ALTERED_SEARCH_PATH) && (fl & LLF_SEARCH_MASK)) ||
        ((fl & LLF_SEARCH_DLL_LOAD_DIR) && !rq->kpath)) {
        *err = W32E_INVALID_PARAMETER;
        return 0;
    }
    char name[WIN32_MT_NAME_MAX];
    if (pe_module_name(spec, name, sizeof name) < 0) { *err = W32E_MOD_NOT_FOUND; return 0; }
    struct pe_ctx *c = pe_rt_begin(err);
    if (!c) return 0;

    /* The search order for this call (and the DLLs it pulls in). */
    if (rq->kpath) (void)pe_split(rq->kpath, c->load_dir, sizeof c->load_dir);
    if (fl & LLF_SEARCH_MASK) {
        if (fl & LLF_SEARCH_DEFAULT_DIRS)
            fl |= LLF_SEARCH_APPLICATION_DIR | LLF_SEARCH_USER_DIRS | LLF_SEARCH_SYSTEM32;
        if (fl & LLF_SEARCH_DLL_LOAD_DIR)    c->dirs[c->ndirs++] = c->load_dir;
        if (fl & LLF_SEARCH_APPLICATION_DIR) c->dirs[c->ndirs++] = c->exe_dir;
        /* LLF_SEARCH_USER_DIRS: AddDllDirectory's list -- there is none yet */
        if (fl & LLF_SEARCH_SYSTEM32)        c->dirs[c->ndirs++] = "/data/Windows/System32";
    } else {
        c->dirs[c->ndirs++] = ((fl & LLF_WITH_ALTERED_SEARCH_PATH) && rq->kpath) ? c->load_dir
                                                                               : c->exe_dir;
        c->dirs[c->ndirs++] = "/data/Windows/System32";
        c->dirs[c->ndirs++] = "/data/Windows";
        c->dirs[c->ndirs++] = c->cwd;
    }

    const char *who = c->mod[0].name;
    bool bare = !rq->kpath;
    for (const char *q = spec; bare && *q; q++) if (*q == '/') bare = false;
    int mi = bare ? pe_loaded(c, name) : -1;      /* a bare name: the loaded list first */
    if (mi < 0) {
        char path[ABI_PATH_MAX], dir[ABI_PATH_MAX], wp[ABI_PATH_MAX];
        void *img = 0;
        size_t sz = 0;
        int borrowed = 0;
        bool got = false;
        if (rq->kpath) {
            (void)pe_split(rq->kpath, dir, sizeof dir);
            got = pe_try_dir(dir[0] ? dir : "/", name, path, sizeof path, &img, &sz, &borrowed);
        } else {
            /* "If a relative path is specified, the entire relative path is
             * appended to every token in the DLL search path." */
            char rel[ABI_PATH_MAX];
            (void)pe_split(spec, rel, sizeof rel);
            for (int i = 0; i < c->ndirs && !got; i++) {
                if (!c->dirs[i] || !c->dirs[i][0]) continue;
                const char *d = c->dirs[i];
                if (rel[0]) {
                    ksnprintf(dir, sizeof dir, "%s%s%s", d, d[strlen(d) - 1] == '/' ? "" : "/", rel);
                    d = dir;
                }
                got = pe_try_dir(d, name, path, sizeof path, &img, &sz, &borrowed);
            }
        }
        if (!got) {
            kprintf("[pe] LoadLibrary(%s): not found\n", spec);
            *err = W32E_MOD_NOT_FOUND;
            pe_rt_end(c);
            return 0;
        }
        /* the same file already loaded (a path that names it, not its name) */
        if (win32_kpath_to_winpath(path, wp, sizeof wp) > 0) mi = pe_loaded_path(c, wp);
        if (mi >= 0) {
            if (!borrowed) kfree(img);
        } else {
            mi = pe_map_file(c, name, path, img, sz, borrowed, who);
        }
    }
    if (c->failed || mi < 0) {
        pe_rt_rollback(c);
        *err = pe_rt_status_err(c->fail_status);
        pe_rt_end(c);
        return 0;
    }
    pe_ref(c, mi);                                /* this LoadLibrary's reference */

    /* the calls to make: the DLLs this call mapped, dependencies first, each
     * its TLS callbacks then its DllMain (lpReserved NULL: a dynamic load) */
    uint64_t b[PE_LIST_WORDS];
    b[0] = 0;
    for (int k = 0; k < c->ninit; k++)
        pe_note(c, &c->mod[c->init[k]], b, PE_DLL_PROCESS_ATTACH | WIN32_LDR_CTL_CHECK, true, true);
    if (!pe_list_put(ubuf, b)) {
        pe_rt_rollback(c);
        *err = W32E_INVALID_PARAMETER;
        pe_rt_end(c);
        return 0;
    }
    pe_commit(c);
    uint64_t base = c->mod[mi].base;
    kprintf("[pe] LoadLibrary(%s) -> %s at %p, refs %u, %d to initialize\n", spec,
            c->mod[mi].name, (void *)base, c->mod[mi].refs, c->ninit);
    pe_rt_end(c);
    return base;
}

uint64_t pe_rt_attached(uint64_t hmod, int64_t failed, uint64_t ubuf) {
    uint32_t err;
    uint64_t zero = 0;
    struct pe_ctx *c = pe_rt_begin(&err);
    if (!c) {
        (void)copy_to_user((void *)(uintptr_t)ubuf, &zero, 8);
        return failed < 0 ? hmod : 0;
    }
    uint64_t b[PE_LIST_WORDS];
    int n = pe_rt_get_list(ubuf, b);
    /* the modules whose calls were made -- the one that refused too: Windows
     * calls it again with DLL_PROCESS_DETACH -- numbered in that order (W5) */
    int upto = (failed < 0 || failed >= n) ? n : (int)failed + 1;
    for (int k = 0; k < upto; k++) {
        int j = pe_by_base(c, b[2 + 3 * k]);
        if (j >= 0 && !(c->mod[j].flags & WIN32_MTM_FLAG_INITED)) {
            c->mod[j].flags |= WIN32_MTM_FLAG_INITED;
            c->mod[j].init_seq = ++c->init_seq;
        }
    }
    if (failed < 0) {
        pe_commit(c);
        pe_rt_end(c);
        return hmod;
    }
    /* A DllMain refused: drop this load's reference. */
    int list[WIN32_MAX_MODULES], nl = 0;
    int mi = pe_by_base(c, hmod);
    if (mi >= 0) pe_rt_release(c, mi, list, &nl);
    kprintf("[pe] LoadLibrary: DllMain %d of %d refused DLL_PROCESS_ATTACH -- unloading %d module(s)\n",
            (int)failed, n, nl);
    if (!pe_rt_put_list(c, ubuf, list, nl)) (void)copy_to_user((void *)(uintptr_t)ubuf, &zero, 8);
    pe_commit(c);
    pe_rt_end(c);
    return 0;
}

int pe_rt_free(uint64_t hmod, uint64_t ubuf, uint32_t *err) {
    struct pe_ctx *c = pe_rt_begin(err);
    if (!c) return 0;
    int mi = pe_by_base(c, hmod);
    if (mi < 0) {
        *err = W32E_MOD_NOT_FOUND;
        pe_rt_end(c);
        return 0;
    }
    int list[WIN32_MAX_MODULES], nl = 0;
    pe_rt_release(c, mi, list, &nl);
    if (nl && !pe_rt_put_list(c, ubuf, list, nl)) {
        *err = W32E_INVALID_PARAMETER;
        pe_rt_end(c);
        return 0;
    }
    pe_commit(c);
    if (nl)
        kprintf("[pe] FreeLibrary(%s): unloading %d module(s)\n", c->mod[mi].name, nl);
    pe_rt_end(c);
    return nl ? 2 : 1;
}

void pe_rt_unmap(uint64_t ubuf) {
    uint32_t err;
    struct pe_ctx *c = pe_rt_begin(&err);
    if (!c) return;
    uint64_t b[PE_LIST_WORDS];
    int n = pe_rt_get_list(ubuf, b);
    for (int k = 0; k < n; k++) {
        int j = pe_by_base(c, b[2 + 3 * k]);
        if (j < 0 || !(c->mod[j].flags & WIN32_MTM_FLAG_UNLOAD)) continue;
        struct pe_mod *m = &c->mod[j];
        pe_tls_release(c, m);
        mmap_unmap_loader_pages(m->base, m->size);
        kprintf("[pe] unloaded %s from %p\n", m->name, (void *)m->base);
        memset(m, 0, sizeof *m);
    }
    while (c->n > 1 && !c->mod[c->n - 1].base) c->n--;
    pe_commit(c);
    pe_rt_end(c);
}

uint64_t pe_rt_find(const char *name, const char *winpath) {
    uint32_t err;
    struct pe_ctx *c = pe_rt_begin(&err);
    if (!c) return 0;
    int mi = -1;
    char key[WIN32_MT_NAME_MAX];
    if (winpath) mi = pe_loaded_path(c, winpath);
    else if (name && pe_module_name(name, key, sizeof key) >= 0) mi = pe_loaded(c, key);
    uint64_t base = mi >= 0 ? c->mod[mi].base : 0;
    pe_rt_end(c);
    return base;
}

uint64_t pe_rt_proc(uint64_t base, const char *name, uint32_t ord, uint32_t *err) {
    struct pe_ctx *c = pe_rt_begin(err);
    if (!c) return 0;
    c->no_load = true;
    int mi = pe_by_base(c, base);
    uint64_t va = 0;
    if (mi < 0) *err = W32E_MOD_NOT_FOUND;
    else if (!(va = pe_export_lookup(c, mi, name, ord, 0))) *err = W32E_PROC_NOT_FOUND;
    pe_rt_end(c);
    return va;
}

int pe_rt_module_path(uint64_t base, char *out, int cap) {
    uint32_t err;
    struct pe_ctx *c = pe_rt_begin(&err);
    if (!c) return -1;
    int mi = pe_by_base(c, base), n = -1;
    if (mi >= 0 && c->mod[mi].path_va)
        n = pe_ustr(c, out, cap, c->mod[mi].path_va, c->mod[mi].path_va + WIN32_MT_PATH_MAX);
    pe_rt_end(c);
    return n;
}

/* ===== W5: the DLL notifications =========================================
 * Each builds a list for a stub (pe.h); the caller (src/syscall.c) holds the
 * loader lock from the list's making to the stub's last call. */

/* The loaded DLLs that were initialized and are not being unloaded, by their
 * initialization numbers: ascending, or (`down`) descending. */
static int pe_inited(const struct pe_ctx *c, int *out, bool down) {
    int n = 0;
    for (int i = 1; i < c->n; i++) {
        const struct pe_mod *m = &c->mod[i];
        if (!m->base || !m->is_dll || !(m->flags & WIN32_MTM_FLAG_INITED) ||
            (m->flags & WIN32_MTM_FLAG_UNLOAD))
            continue;
        int k = n++;
        while (k > 0 && (down ? c->mod[out[k - 1]].init_seq < m->init_seq
                              : c->mod[out[k - 1]].init_seq > m->init_seq)) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = i;
    }
    return n;
}

/* The DLLs the module table's init list names, in its order (the order they
 * were bound: a DLL after the DLLs it imports). */
static int pe_init_list(const struct pe_ctx *c, int *out) {
    uint32_t ninit = 0;
    if (!pe_uread(c, &ninit, WIN32_MODTAB_BASE + WIN32_MT_INIT_COUNT, 4) || ninit > WIN32_MAX_MODULES)
        return 0;
    int n = 0;
    for (uint32_t i = 0; i < ninit; i++) {
        uint64_t e[3];
        if (!pe_uread(c, e, WIN32_MODTAB_BASE + WIN32_MT_INIT_LIST + (uint64_t)i * WIN32_MT_INIT_STRIDE,
                      sizeof e))
            break;
        int j = pe_by_base(c, e[1]);
        if (j > 0 && c->mod[j].is_dll) out[n++] = j;
    }
    return n;
}

static void pe_list_clear(uint64_t ubuf) {
    uint64_t zero = 0;
    (void)copy_to_user((void *)(uintptr_t)ubuf, &zero, 8);
}

int pe_rt_start(uint64_t ubuf) {
    pe_list_clear(ubuf);
    uint32_t err;
    struct pe_ctx *c = pe_rt_begin(&err);
    if (!c) return -1;
    int order[WIN32_MAX_MODULES];
    int n = pe_init_list(c, order);
    uint64_t b[PE_LIST_WORDS];
    b[0] = 0;
    for (int k = 0; k < n; k++) {
        const struct pe_mod *m = &c->mod[order[k]];
        if (m->flags & WIN32_MTM_FLAG_INITED) continue;
        pe_note(c, m, b, PE_DLL_PROCESS_ATTACH | WIN32_LDR_CTL_RES | WIN32_LDR_CTL_CHECK, true, true);
    }
    pe_note(c, &c->mod[0], b, PE_DLL_PROCESS_ATTACH | WIN32_LDR_CTL_RES, false, false);
    bool ok = pe_list_put(ubuf, b);
    pe_rt_end(c);
    return ok ? (int)b[0] : -1;
}

uint64_t pe_rt_started(int64_t failed, uint64_t ubuf, char *who, int cap) {
    uint32_t err;
    struct pe_ctx *c = pe_rt_begin(&err);
    if (cap > 0) who[0] = '\0';
    if (!c) return 0;
    uint64_t b[PE_LIST_WORDS];
    int n = pe_rt_get_list(ubuf, b);
    if (failed >= 0) {
        int j = failed < n ? pe_by_base(c, b[2 + 3 * failed]) : -1;
        if (j >= 0 && cap > 0) ksnprintf(who, (size_t)cap, "%s", c->mod[j].name);
        pe_rt_end(c);
        return 0;
    }
    /* every DLL of the init list is initialized: number them in its order */
    int order[WIN32_MAX_MODULES];
    int k = pe_init_list(c, order);
    for (int i = 0; i < k; i++) {
        struct pe_mod *m = &c->mod[order[i]];
        if (m->flags & WIN32_MTM_FLAG_INITED) continue;
        m->flags |= WIN32_MTM_FLAG_INITED;
        m->init_seq = ++c->init_seq;
    }
    uint64_t entry = 0;
    (void)pe_uread(c, &entry, WIN32_MODTAB_BASE + WIN32_MT_EXE_ENTRY, 8);
    pe_commit(c);
    pe_rt_end(c);
    return entry;
}

int pe_rt_thread_list(uint32_t reason, uint64_t ubuf) {
    pe_list_clear(ubuf);
    uint32_t err;
    struct pe_ctx *c = pe_rt_begin(&err);
    if (!c) return -1;
    int order[WIN32_MAX_MODULES];
    int n = pe_inited(c, order, reason == PE_DLL_THREAD_DETACH);
    uint64_t b[PE_LIST_WORDS];
    b[0] = 0;
    for (int k = 0; k < n; k++) {
        const struct pe_mod *m = &c->mod[order[k]];
        pe_note(c, m, b, reason, !(m->flags & WIN32_MTM_FLAG_NOTHREAD), false);
    }
    pe_note(c, &c->mod[0], b, reason, false, false);
    bool ok = pe_list_put(ubuf, b);
    pe_rt_end(c);
    return ok ? (int)b[0] : -1;
}

int pe_rt_exit_list(uint64_t ubuf) {
    pe_list_clear(ubuf);
    uint32_t err;
    struct pe_ctx *c = pe_rt_begin(&err);
    if (!c) return -1;
    int order[WIN32_MAX_MODULES];
    int n = pe_inited(c, order, true);
    uint64_t b[PE_LIST_WORDS];
    b[0] = 0;
    for (int k = 0; k < n; k++)
        pe_note(c, &c->mod[order[k]], b, PE_DLL_PROCESS_DETACH | WIN32_LDR_CTL_RES, true, false);
    pe_note(c, &c->mod[0], b, PE_DLL_PROCESS_DETACH | WIN32_LDR_CTL_RES, false, false);
    bool ok = pe_list_put(ubuf, b);
    pe_rt_end(c);
    return ok ? (int)b[0] : -1;
}

int pe_rt_disable_thread_calls(uint64_t hmod) {
    uint32_t err;
    struct pe_ctx *c = pe_rt_begin(&err);
    if (!c) return 0;
    int mi = pe_by_base(c, hmod);
    if (mi < 0) {
        pe_rt_end(c);
        return 0;
    }
    struct pe_tls_dir64 t;
    struct pe_mod *m = &c->mod[mi];
    if (m->is_dll && !(m->flags & WIN32_MTM_FLAG_NOTHREAD) && !pe_tls_dir(c, m, &t)) {
        m->flags |= WIN32_MTM_FLAG_NOTHREAD;
        pe_commit(c);
    }
    pe_rt_end(c);
    return 1;
}

int pe_rt_tls_templates(struct pe_tls_tmpl *out, int cap) {
    uint32_t err;
    struct pe_ctx *c = pe_rt_begin(&err);
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->n && n < cap; i++) {
        const struct pe_mod *m = &c->mod[i];
        if (!m->base || (m->flags & WIN32_MTM_FLAG_UNLOAD) || (i != 0 && !m->tls_slot)) continue;
        struct pe_tls_dir64 t;
        if (!pe_tls_dir(c, m, &t)) continue;
        uint64_t raw = (t.raw_end > t.raw_start) ? (t.raw_end - t.raw_start) : 0;
        uint64_t total = raw + (uint64_t)t.zero_fill;
        if (t.raw_start < m->base || t.raw_start + raw > m->base + m->size || total > WIN32_TLS_MAX_BLK)
            continue;                         /* as pe_setup_tls skipped it */
        out[n].slot = (uint32_t)(i == 0 ? 0 : m->tls_slot);
        out[n].raw = t.raw_start;
        out[n].raw_size = raw;
        out[n].total = round_up(total, 16);
        n++;
    }
    pe_rt_end(c);
    return n;
}

/* W6: one argument of a command line, written at p (in a uaccess window, up
 * to lim): as it is when nothing in it needs quoting, else in double quotes
 * with a quote as \" and the backslashes before a quote, or before the
 * closing one, doubled -- the inverse of the C runtime's parse. The program
 * name (argv[0]) is only quoted: its rules have no escapes. */
static uint64_t pe_cmd_quote(uint64_t p, uint64_t lim, const char *s, bool prog) {
#define PUTC(c) do { if (p < lim) *(char *)(p++) = (c); } while (0)
    bool plain = s[0] != '\0';
    for (const char *q = s; *q; q++)
        if (*q == ' ' || *q == '\t' || (*q == '"' && !prog)) plain = false;
    if (plain) {
        for (; *s; s++) PUTC(*s);
        return p;
    }
    PUTC('"');
    if (prog) {
        for (; *s; s++) if (*s != '"') PUTC(*s);
        PUTC('"');
        return p;
    }
    for (;;) {
        int nbs = 0;
        while (*s == '\\') { s++; nbs++; }
        if (!*s) { for (int i = 0; i < 2 * nbs; i++) PUTC('\\'); break; }
        if (*s == '"') {
            for (int i = 0; i < 2 * nbs + 1; i++) PUTC('\\');
            PUTC('"');
            s++;
            continue;
        }
        for (int i = 0; i < nbs; i++) PUTC('\\');
        PUTC(*s);
        s++;
    }
    PUTC('"');
#undef PUTC
    return p;
}

int pe_load_user(const void *image, size_t size, int argc, char **argv,
                 const struct pe_load_env *env, struct pe_load_info *out) {
    if (!image || !out || !pe_is_image(image, size)) return -1;
    out->entry = 0;
    out->image_base = 0;

    struct pe_hdr h;
    if (!pe_parse(image, size, &h)) {
        kprintf("[pe] reject: not a PE32+ x64 image (or its headers run off it)\n");
        return -1;
    }
    const struct coff_header     *coff = h.coff;
    const struct pe_opt_header64 *opt  = h.opt;
    const struct pe_data_dir     *dirs = h.dirs;

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
    int64_t  delta     = 0;                                   /* 0 == load at ImageBase */

    /* ---- C24: ASLR / base relocation -----------------------------------
     * If the image opted into dynamic basing (linked with --dynamicbase, which
     * sets IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE and keeps the .reloc table),
     * load it at a RANDOMIZED, 64KB-aligned slide instead of its preferred
     * ImageBase and let pe_place rewrite every absolute address via the base-reloc
     * fixups. The slide stays in [16 MiB, ~80 MiB) above ImageBase: a mingw EXE
     * bases at ~5 GiB, so the slid image stays far above the fixed shim/TEB/TLS/
     * CRT pages (all < 0x32000000) and far below the user stack (0x800000000000). */
    bool has_reloc = (opt->num_data_dirs > PE_DIR_BASERELOC) &&
                     dirs[PE_DIR_BASERELOC].rva && dirs[PE_DIR_BASERELOC].size;
    if ((opt->dll_characteristics & PE_DLLCHAR_DYNAMIC_BASE) && has_reloc) {
        uint64_t r     = (perf_rdtsc() >> 8) & 0x3FFu;        /* 0..1023 */
        uint64_t slide = 0x01000000ULL + (r << 16);           /* 16 MiB + n*64 KiB */
        if (image_base + slide + size_of_image <= 0x0000800000000000ULL) {
            load_base = image_base + slide;
            delta     = (int64_t)(load_base - image_base);
        }
    }

    kprintf("[pe] PE32+ base=%p size=0x%lx entry_rva=0x%x sections=%u dirs=%u\n",
            (void *)load_base, (unsigned long)size_of_image,
            opt->entry_point_rva, coff->num_sections, opt->num_data_dirs);
    if (delta != 0)
        kprintf("[pe] C24 ASLR: ImageBase=%p -> load_base=%p slide=0x%lx "
                "(reloc dir rva=0x%x size=0x%x)\n",
                (void *)image_base, (void *)load_base, (unsigned long)delta,
                dirs[PE_DIR_BASERELOC].rva, dirs[PE_DIR_BASERELOC].size);

    /* ---- 1. map the image span, the shim area, the TEB, the CRT data page
     * and (W3) the module table (all RW, user) ---- */
    if (!pe_map_user(load_base, load_base + size_of_image)) {
        kprintf("[pe] failed mapping image pages\n");
        return -1;
    }
    if (!pe_map_user(WIN32_SHIM_BASE, WIN32_SHIM_BASE + WIN32_SHIM_BYTES)) {
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
    if (!pe_map_user(WIN32_MODTAB_BASE, WIN32_MODTAB_BASE + WIN32_MODTAB_BYTES)) {
        kprintf("[pe] failed mapping the module table\n");
        return -1;
    }

    /* ---- minimal TEB ---- the CRT startup reads gs:[0x30] (NtCurrentTeb).
     * Zero the page, then set the self-pointer + stack bounds. The proc's GS
     * base is set to WIN32_TEB_BASE by the spawn path, and the SWAPGS scheme
     * makes gs:[...] resolve here while the PE runs in CPL3. */
    /* W7d-2: the main thread's stack is a reservation of the .exe's
     * SizeOfStackReserve -- at least 512 KiB (spawn maps 256 KiB of it at
     * once, and the guard and some room must lie below that), at most the
     * 8 MiB the stack growth path reaches -- its lowest 16 KiB the guard
     * (page_fault.c grows it no further than the guard's top: the spawn path
     * sets p->win_stack_floor from out->stack_reserve). */
    uint64_t sres = (opt->stack_reserve + 0xFFFFull) & ~0xFFFFull;
    if (sres < 0x80000ull) sres = 0x80000ull;
    if (sres > 0x800000ull) sres = 0x800000ull;
    out->stack_reserve = sres;
    {
        unsigned long uf = uaccess_begin();
        memset((void *)WIN32_TEB_BASE, 0, PAGE_SIZE);
        *(uint64_t *)(WIN32_TEB_BASE + WIN32_TEB_SELF)       = WIN32_TEB_BASE;
        *(uint64_t *)(WIN32_TEB_BASE + WIN32_TEB_STACKBASE)  = WIN32_USER_STACK_TOP;
        *(uint64_t *)(WIN32_TEB_BASE + WIN32_TEB_STACKLIMIT) = WIN32_USER_STACK_TOP - sres + WIN32_STACK_GUARD_BYTES;
        *(uint64_t *)(WIN32_TEB_BASE + WIN32_TEB_STACK_BOTTOM) = WIN32_USER_STACK_TOP - sres;
        memset((void *)WIN32_MODTAB_BASE, 0, WIN32_MODTAB_BYTES);
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

        /* C7, W6: _acmdln and GetCommandLine -- the command line. The GUI CRT
         * startup reads it via __p__acmdln to build WinMain's lpCmdLine. W6:
         * the exact string, when a CreateProcess call is starting this
         * program (win32_spawn_cmdline: its thread is the current one here);
         * otherwise every argument, quoted by the rules the CRT parses them
         * back with (Microsoft's "Parsing C command-line arguments"), as the
         * POSIX side's launchers of Windows programs build it. Until W6 it was
         * argv[0] alone. Its area of the page ends at WIN32_CRT_NMHDR. */
        {
            uint64_t cl = base + WIN32_CRT_CMDLINE, lim = base + WIN32_CRT_NMHDR - 1;
            uint64_t clp = cl;
            const char *raw = win32_spawn_cmdline();
            if (raw) {
                while (*raw && clp < lim) *(char *)(clp++) = *raw++;
            } else {
                for (int i = 0; argv && i < argc && argv[i]; i++) {
                    if (i && clp < lim) *(char *)(clp++) = ' ';
                    clp = pe_cmd_quote(clp, lim, argv[i], i == 0);
                }
            }
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

    /* ---- 2. copy headers + sections into the mapped pages; 3. base
     * relocations (only if we couldn't honour ImageBase) ---- */
    pe_place(image, size, &h, load_base, delta, size_of_image);

    /* ---- 4. the shim area, then the .exe's imports -- and (W3) the DLLs they
     * name, each bound the same way -- then TLS and W^X per module ---- */
    pe_setup_shim_area();

    struct pe_ctx *c = kmalloc(sizeof *c);
    if (!c) {
        kprintf("[pe] out of memory for the loader's state\n");
        return -1;
    }
    memset(c, 0, sizeof *c);
    c->out       = out;
    c->cwd       = (env && env->cwd) ? env->cwd : "/";
    c->next_base = 0x0000000200000000ULL;
    c->tls_next  = WIN32_TLS_BASE + WIN32_TLS_BLK_OFF;
    c->tls_end   = WIN32_TLS_BASE;
    c->tls_used  = 1u;                            /* slot 0 is the .exe's */
    out->tls_raw_va = out->tls_raw_size = out->tls_total = out->tls_index = 0;
    {
        const char *ep = (env && env->exe_kpath) ? env->exe_kpath : "/";
        int last = 0;
        for (int i = 0; ep[i]; i++) if (ep[i] == '/') last = i;
        int dn = last > 0 ? last : 1;
        if (dn >= (int)sizeof c->exe_dir) dn = (int)sizeof c->exe_dir - 1;
        memcpy(c->exe_dir, ep, (size_t)dn);
        c->exe_dir[dn] = '\0';

        struct pe_mod *x = &c->mod[0];
        const char *bn = ep + last + (ep[last] == '/' ? 1 : 0);
        int k = 0;
        for (; bn[k] && k < (int)sizeof x->name - 1; k++) x->name[k] = pe_lower(bn[k]);
        x->name[k] = '\0';
        x->base  = load_base;
        x->size  = size_of_image;
        x->entry = load_base + opt->entry_point_rva;
        struct pe_data_dir d;
        d = pe_dir(&h, PE_DIR_EXPORT); x->export_rva = d.rva; x->export_size = d.size;
        d = pe_dir(&h, PE_DIR_IMPORT); x->import_rva = d.rva; x->import_size = d.size;
        d = pe_dir(&h, PE_DIR_TLS);    x->tls_rva = d.rva;    x->tls_size = d.size;
        x->is_dll = false;
        x->flags  = WIN32_MTM_FLAG_PINNED;
        c->n = 1;
        char wp[ABI_PATH_MAX];
        bool have_wp = env && env->exe_kpath && win32_kpath_to_winpath(env->exe_kpath, wp, sizeof wp) > 0;
        pe_slot_strings(c, 0, have_wp ? wp : 0);
        /* the DLL search order (W4: LoadLibraryEx builds its own per call) */
        c->dirs[0] = c->exe_dir;
        c->dirs[1] = "/data/Windows/System32";
        c->dirs[2] = "/data/Windows";
        c->dirs[3] = c->cwd;
        c->ndirs = 4;
    }
    {
        unsigned long uf = uaccess_begin();
        pe_missing_init(&c->miss);
        uaccess_end(uf);
        int li = win32_shim_index("kernel32.dll", "__toby_loadfail");
        uint64_t lf = (li >= 0 && (uint64_t)li < WIN32_PA_CAP)
            ? WIN32_PROCADDR_BASE_VA + (uint64_t)li * WIN32_PROCADDR_STRIDE : 0;
        (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_LOADFAIL, &lf, 8);
        /* W4: the LoadLibrary/FreeLibrary stubs' way into the kernel */
        int ri = win32_shim_index("kernel32.dll", "__toby_ldr");
        uint64_t ldr = (ri >= 0 && (uint64_t)ri < WIN32_PA_CAP)
            ? WIN32_PROCADDR_BASE_VA + (uint64_t)ri * WIN32_PROCADDR_STRIDE : 0;
        (void)pe_uwrite(c, WIN32_MODTAB_BASE + WIN32_MT_LDR, &ldr, 8);
    }

    (void)pe_bind_module(c, 0);
    uint64_t exe_entry = load_base + opt->entry_point_rva;
    pe_write_modtab(c, exe_entry);

    /* ---- 5. W^X for the fixed pages ---- */
    /* shim region: executable, read-only (no write, no NX) at CPL3 */
    (void)vmm_protect(WIN32_SHIM_BASE, WIN32_SHIM_BYTES, VMM_PRESENT | VMM_USER);
    /* TEB + CRT data pages: data, read-write, never executable */
    (void)vmm_protect(WIN32_TEB_BASE, PAGE_SIZE, VMM_PRESENT | VMM_USER | VMM_WRITE | VMM_NX);
    (void)vmm_protect(WIN32_CRT_DATA_BASE, PAGE_SIZE, VMM_PRESENT | VMM_USER | VMM_WRITE | VMM_NX);
    /* W3: the module table (W4's runtime loads update it) and the TLS area */
    (void)vmm_protect(WIN32_MODTAB_BASE, WIN32_MODTAB_BYTES, VMM_PRESENT | VMM_USER | VMM_WRITE | VMM_NX);
    if (c->tls_end > WIN32_TLS_BASE)
        (void)vmm_protect(WIN32_TLS_BASE, c->tls_end - WIN32_TLS_BASE,
                          VMM_PRESENT | VMM_USER | VMM_WRITE | VMM_NX);

    out->image_base = load_base;
    /* W5: a process with DLLs from disk, or with TLS callbacks of its own
     * (every mingw .exe has the CRT's), starts at the stub that calls them */
    out->entry      = c->failed ? WIN32_FAIL_ENTRY_VA
                    : (c->ninit || pe_tls_callbacks(c, &c->mod[0])) ? WIN32_LDR_START_VA
                                : exe_entry;
    out->teb        = WIN32_TEB_BASE;
    out->crt_data   = WIN32_CRT_DATA_BASE;
    /* C14: expose the resource directory (.rsrc) so DialogBoxParamA can walk it
     * for RT_DIALOG templates. rsrc_rva is a directory offset relative to the
     * loaded image base; the kernel adds load_base when parsing. */
    if (opt->num_data_dirs > PE_DIR_RESOURCE && dirs[PE_DIR_RESOURCE].rva) {
        out->rsrc_rva  = (uint64_t)dirs[PE_DIR_RESOURCE].rva;
        out->rsrc_size = (uint64_t)dirs[PE_DIR_RESOURCE].size;
    } else {
        out->rsrc_rva = out->rsrc_size = 0;
    }
    kprintf("[pe] loaded: base=%p entry=%p (gate@%p teb@%p crt@%p) modules=%d init=%d%s\n",
            (void *)load_base, (void *)out->entry,
            (void *)WIN32_SHIM_BASE, (void *)WIN32_TEB_BASE,
            (void *)WIN32_CRT_DATA_BASE, c->n, c->ninit,
            c->failed ? " LOAD FAILED" : "");
    kfree(c);
    return 0;
}
