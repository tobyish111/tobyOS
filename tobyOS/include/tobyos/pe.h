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
    uint64_t teb;          /* user VA of the Thread Environment Block (gs base) */
    uint64_t crt_data;     /* user VA of the CRT data page (argc/argv/environ/...) */
    uint64_t rsrc_rva;     /* C14: .rsrc directory RVA (0 if none) -- dialog templates */
    uint64_t rsrc_size;    /* C14: .rsrc directory size                             */
    /* C18b: .tls (IMAGE_TLS_DIRECTORY) template, so each CreateThread thread can
     * build its own TLS block. All 0 when the PE has no usable .tls directory. */
    uint64_t tls_raw_va;   /* VA of the template raw bytes (StartAddressOfRawData) */
    uint64_t tls_raw_size; /* template raw byte count (End - Start)                */
    uint64_t tls_total;    /* per-thread block size (raw + zero-fill, 16-aligned)  */
    uint64_t tls_index;    /* the module's assigned _tls_index (0 = sole module)   */
};

/* True if [image,size) looks like a PE/COFF image: 'MZ' DOS magic and a
 * 'PE\0\0' signature at e_lfanew. Cheap sniff used by the exec path to
 * branch ELF vs PE. */
bool pe_is_image(const void *image, size_t size);

/* W3: where the .exe came from and the process's working directory -- the
 * DLL search order's first and last places. */
struct pe_load_env {
    const char *exe_kpath;     /* the .exe's tobyOS path, e.g. "/bin/win-dll28.exe" */
    const char *cwd;           /* the new process's working directory */
};

/* Map a PE32+ image into the *currently active* user address space. The
 * caller (spawn) must already have installed the child's CR3 + vmm editor
 * root so vmm_map edits + user writes land in the child. Applies base
 * relocations, installs the Win32 IAT thunks + marshalling gate, loads (W3)
 * the DLLs its imports name, and reports the entry point -- the init
 * trampoline when there are DLLs to initialize, the fail entry when one could
 * not be loaded. Returns 0 on success, <0 when the .exe itself is unusable. */
int pe_load_user(const void *image, size_t size, int argc, char **argv,
                 const struct pe_load_env *env, struct pe_load_info *out);

/* W3: the module table -- the loader's record of the modules it mapped, in
 * user memory as Windows keeps its loader data (the PEB's). Fixed VA, 2 pages:
 * a header, the init list (one 24-byte entry per DLL to call at
 * DLL_PROCESS_ATTACH, in order), the module records (64 bytes each, the .exe
 * first) and the strings they point at. The init trampoline and the fail entry
 * hard-code the VA and the header offsets. W4: the table is the loader's state
 * between calls too -- LoadLibrary and FreeLibrary read it, change it and write
 * it back -- so a record carries its reference count, a slot's strings have a
 * fixed place (a freed slot is reused without growing anything), and the
 * cursors of the missing-import area and the TLS area live in it. Being user
 * memory, it is checked on every read: a program that scribbles on it can make
 * its own LoadLibrary fail, never the kernel. */
#define WIN32_MODTAB_BASE      0x0000000031008000ULL
#define WIN32_MODTAB_BYTES     (2u * 4096u)
#define WIN32_MAX_MODULES      16
#define WIN32_MT_EXE_ENTRY     0x000   /* u64: the .exe's entry point          */
#define WIN32_MT_LOADFAIL      0x008   /* u64: __toby_loadfail's thunk         */
#define WIN32_MT_INIT_COUNT    0x010   /* u32: entries in the init list        */
#define WIN32_MT_FAIL_TEXT     0x018   /* u64: VA of the load failure's text   */
#define WIN32_MT_FAIL_STATUS   0x020   /* u32: its NTSTATUS                    */
#define WIN32_MT_MOD_COUNT     0x024   /* u32: record slots in use (high water) */
#define WIN32_MT_LDR           0x028   /* u64: W4: __toby_ldr's thunk          */
#define WIN32_MT_INIT_LIST     0x040   /* {u64 entry, u64 base, u64 name VA}   */
#define WIN32_MT_INIT_STRIDE   24
#define WIN32_MT_MISS_STUB     0x1C0   /* u64: W4: next free missing-import stub */
#define WIN32_MT_MISS_NAME     0x1C8   /* u64: W4: next free byte of their names */
#define WIN32_MT_TLS_NEXT      0x1D0   /* u64: W4: next free byte of the TLS area */
#define WIN32_MT_TLS_END       0x1D8   /* u64: W4: end of the mapped TLS area    */
#define WIN32_MT_TLS_USED      0x1E0   /* u32: W4: TLS slots in use (bit n = n)  */
#define WIN32_MT_MODS          0x200   /* module records                       */
#define WIN32_MT_MOD_STRIDE    64
#define WIN32_MT_STRINGS       0x600   /* W4: each slot's name + Windows path  */
#define WIN32_MT_STR_STRIDE    0x180   /*   name at +0, path at +WIN32_MT_NAME_MAX */
#define WIN32_MT_NAME_MAX      64
#define WIN32_MT_PATH_MAX      (WIN32_MT_STR_STRIDE - WIN32_MT_NAME_MAX)
#define WIN32_MT_FAIL_AREA     0x1E00  /* W4: the process-start failure's text */
#define WIN32_MTM_BASE         0x00    /* u64 */
#define WIN32_MTM_SIZE         0x08    /* u64 */
#define WIN32_MTM_ENTRY        0x10    /* u64: 0 if none */
#define WIN32_MTM_NAME         0x18    /* u64: VA of the lower-case file name */
#define WIN32_MTM_PATH         0x20    /* u64: VA of its Windows path */
#define WIN32_MTM_EXPORT_RVA   0x28    /* u32 */
#define WIN32_MTM_EXPORT_SIZE  0x2C    /* u32 */
#define WIN32_MTM_FLAGS        0x30    /* u32 */
#define WIN32_MTM_REFS         0x34    /* u32: W4: LoadLibrary + import references */
#define WIN32_MTM_TLS_SLOT     0x38    /* u32: W4: its TLS slot, 0 if none (the .exe's is 0) */
#define WIN32_MTM_FLAG_DLL     1u
#define WIN32_MTM_FLAG_PINNED  2u      /* W4: loaded with the process: never unloaded */
#define WIN32_MTM_FLAG_INITED  4u      /* W4: its DLL_PROCESS_ATTACH was called      */
#define WIN32_MTM_FLAG_UNLOAD  8u      /* W4: between FreeLibrary's DETACH and unmap */

/* W3: CPL3 routines in the shim area's page 0 (the space W1 freed). */
#define WIN32_INIT_TRAMP_VA    0x0000000030000200ULL   /* DllMains, then the .exe */
#define WIN32_FAIL_ENTRY_VA    0x0000000030000280ULL   /* a load failure's entry  */
#define WIN32_ONEXIT_EXEC_VA   0x00000000300002C0ULL   /* _execute_onexit_table   */
/* W4: LoadLibrary and FreeLibrary run in CPL3 (they call DllMain); the source
 * is logs/w32/w4-ldr.S. Each entry point's VA, and the stubs' extent. */
#define WIN32_LDR_STUBS_VA     0x0000000030000300ULL
#define WIN32_LDR_LOADA_VA     (WIN32_LDR_STUBS_VA + 0x00)   /* LoadLibraryA   */
#define WIN32_LDR_LOADW_VA     (WIN32_LDR_STUBS_VA + 0x08)   /* LoadLibraryW   */
#define WIN32_LDR_LOADEXA_VA   (WIN32_LDR_STUBS_VA + 0x18)   /* LoadLibraryExA */
#define WIN32_LDR_LOADEXW_VA   (WIN32_LDR_STUBS_VA + 0x20)   /* LoadLibraryExW */
#define WIN32_LDR_FREE_VA      (WIN32_LDR_STUBS_VA + 0x98)   /* FreeLibrary    */
#define WIN32_LDR_BUF_MAX      WIN32_MAX_MODULES             /* {entry, base} pairs */

#define WIN32_STATUS_DLL_NOT_FOUND        0xC0000135u
#define WIN32_STATUS_DLL_INIT_FAILED      0xC0000142u
#define WIN32_STATUS_INVALID_IMAGE_FORMAT 0xC000007Bu
#define WIN32_STATUS_NO_MEMORY            0xC0000017u

/* W4: the run-time loader (src/pe_loader.c), for the shims in src/syscall.c.
 * Each works on the CURRENT process's module table and returns a Win32 error
 * code through *err (0 on success). A module name is a file name ("x.dll",
 * "x" -- ".dll" is added -- or "x." for no extension), matched without case. */
struct pe_rt_req {
    const char *name;    /* a file name or a relative path ('/'-separated), or NULL */
    const char *kpath;   /* an absolute tobyOS path, or NULL                       */
    uint32_t    flags;   /* LoadLibraryEx's dwFlags                                */
};
/* LoadLibrary's kernel half: the module's base (0 on failure); ubuf gets the
 * entry points the stub must call at DLL_PROCESS_ATTACH. */
uint64_t pe_rt_load(const struct pe_rt_req *rq, uint64_t ubuf, uint32_t *err);
/* After the DllMains: failed = index of the one that returned FALSE, or -1.
 * Returns hmod, or 0 with ubuf = the modules to detach and unmap. */
uint64_t pe_rt_attached(uint64_t hmod, int64_t failed, uint64_t ubuf);
/* FreeLibrary's kernel half: 0 bad handle (*err set), 1 done, 2 ubuf = the
 * modules to detach and then unmap (pe_rt_unmap). */
int pe_rt_free(uint64_t hmod, uint64_t ubuf, uint32_t *err);
/* Unmap the modules ubuf lists that FreeLibrary or a failed load released. */
void pe_rt_unmap(uint64_t ubuf);
/* GetModuleHandle: a loaded module's base by file name or by Windows path
 * (winpath non-NULL: its full path, compared without case), or 0. */
uint64_t pe_rt_find(const char *name, const char *winpath);
/* GetProcAddress: an export of the loaded module at `base`, by name (or, name
 * NULL, by ordinal); 0 if there is no such module (*err 126) or export (127). */
uint64_t pe_rt_proc(uint64_t base, const char *name, uint32_t ord, uint32_t *err);
/* GetModuleFileName: the Windows path of the loaded module at `base`. Returns
 * its length, or -1 if no module is loaded there. */
int pe_rt_module_path(uint64_t base, char *out, int cap);
/* An import from a DLL tobyOS implements itself: its CPL3 stub or its shim's
 * procaddr thunk, or 0 (GetProcAddress on a built-in module uses this). */
uint64_t win32_builtin_proc_va(const char *dll, const char *func);
/* A module name ("x", "x.dll", "dir\x.DLL", "x.") as the loader keys it: the
 * file name, lower case, ".dll" added when there is no extension and a
 * trailing '.' dropped. Returns its length, or -1 if it does not fit. */
int pe_module_name(const char *in, char *out, int cap);

/* W3 (src/syscall.c): 1 if tobyOS implements DLL `dll` itself -- a shim names
 * it, or it is an API-set name (api-ms-win-*, ext-ms-win-*) -- so the loader
 * binds its imports to shims and never looks for a file. */
int win32_dll_is_builtin(const char *dll);
/* W3 (src/syscall.c): a tobyOS path in its Windows form (C: = /data, Z: = /). */
int win32_kpath_to_winpath(const char *kpath, char *out, int cap);

/* CRT data page layout (Track C). The ucrt startup reaches argc/argv/environ
 * through __p___argc / __p___argv / __p__environ, and _commode/_fmode through
 * __p__commode / __p__fmode -- all of which must return USER pointers. The PE
 * loader fills one user page with this fixed layout; the shims return
 * crt_data + the matching offset. */
#define WIN32_CRT_DATA_BASE 0x0000000031001000ULL  /* fixed VA of the page */
#define WIN32_CRT_ARGC      0x00   /* int   argc            */
#define WIN32_CRT_COMMODE   0x04   /* int   _commode        */
#define WIN32_CRT_FMODE     0x08   /* int   _fmode          */
#define WIN32_CRT_ARGV      0x10   /* char**argv  (a value) */
#define WIN32_CRT_ENVIRON   0x18   /* char**environ (value) */
#define WIN32_CRT_ERRNO     0x20   /* int   errno (for _errno())   */
#define WIN32_CRT_WNDPROC   0x28   /* WNDPROC of the (single) window -- the C7
                                    * DispatchMessage trampoline reads [this]  */
#define WIN32_CRT_ACMDLN    0x30   /* char* to the command line (__p__acmdln)  */
#define WIN32_CRT_ARGV_ARR  0x40   /* char* argv[]  array   */
#define WIN32_CRT_ENVIRON_ARR 0x100/* char* environ[] array */
#define WIN32_CRT_STRINGS   0x140  /* argv string bytes     */
/* C4 (C++ CRT): a minimal "C" locale `struct lconv` + the strings the
 * shims hand back. Past the argv strings region. */
#define WIN32_CRT_LCONV     0x300  /* struct lconv (decimal_point first) */
#define WIN32_CRT_LCONV_DOT 0x3F0  /* "." (lconv.decimal_point target)   */
#define WIN32_CRT_STRERR    0x3F8  /* strerror() fixed message           */
/* C5 (file I/O): a scratch buffer where CreateFileA writes the translated
 * (Windows->tobyOS) path so it can hand sys_open a USER pointer. */
#define WIN32_CRT_PATHBUF   0x800  /* translated path scratch (<=0x200)  */
#define WIN32_CRT_CMDLINE   0xA00  /* the command-line string bytes      */
/* C14 (TAB control): a staged NMHDR { HWND hwndFrom; UINT_PTR idFrom; UINT code }
 * whose USER pointer is passed in WM_NOTIFY.lParam (the app reads ->code). */
#define WIN32_CRT_NMHDR     0xC00  /* struct NMHDR (24 bytes)            */
/* C16c: GetCommandLineW builds a UTF-16 copy of the ANSI command line here. */
#define WIN32_CRT_CMDLINEW  0xC80  /* wide command line (<=0x300 bytes)  */

/* C6 (multithreading): the CPL3 thread wrapper lives at this fixed VA in the
 * shim page (WIN32_SHIM_BASE 0x30000000 + WIN32_WRAPPER_OFF 0x80); CreateThread
 * starts each tobyOS thread there. And a Win32 thread HANDLE is tagged so it
 * is distinguishable from a file fd or a ucrt FILE* token. */
#define WIN32_THREAD_WRAPPER_VA  0x0000000030000080ULL
#define WIN32_THREAD_TAG         0x7400000000000000ULL  /* handle = TAG | tid */

/* C7 (GUI): the DispatchMessage trampoline (calls the user WndProc directly in
 * CPL3 -- a kernel shim can't). Lives in the shim page at WIN32_DISPATCH_OFF;
 * its IAT slot is bound to this VA instead of a gate-thunk. */
#define WIN32_DISPATCH_STUB_VA   0x00000000300000C0ULL

/* C14 (dialog templates): a MODAL DialogBoxParamA runs its message loop in CPL3
 * (it dispatches to the app's DialogProc), so its IAT slot is bound to a
 * trampoline at this fixed VA. The trampoline reaches the kernel via the
 * __toby_dlgsvc gate-thunk (also a fixed VA) so the blob needs no patching. */
#define WIN32_DLG_TRAMP_VA       0x0000000030000100ULL
#define WIN32_DLGSVC_THUNK_VA    0x00000000300001F0ULL

/* C19b (real Lua / setjmp-using binaries): setjmp/longjmp save+restore the
 * CALLER's live register context, so they CANNOT be kernel shims (a syscall is
 * a different context). Their IAT slots are bound to pure-CPL3 leaf stubs in the
 * shim page that never trap -- a self-consistent {rbx,rbp,rsp,r12-r15,rsi,rdi,
 * retaddr} save/restore (no SEH unwind; sufficient for Lua's non-local goto).
 * Placed high in page 0, above the import-thunk region. */
#define WIN32_SETJMP_STUB_VA     0x0000000030000E00ULL
#define WIN32_LONGJMP_STUB_VA    0x0000000030000E80ULL

/* C20 (float-return marshalling): the integer gate captures only rcx/rdx/r8/r9 +
 * stack and returns rax, so a `double`-returning Win32/CRT function (libm sqrt,
 * sin, pow, ...) gets garbage in xmm0. A SECOND gate captures xmm0..xmm3 (the
 * MS-x64 FP arg registers) alongside the 4 integer regs into an 8-qword array
 * (ints a[0..3], doubles-as-bits a[4..7]) and, after the dispatch syscall returns
 * the result's bit pattern in rax, does `movq xmm0, rax` so the double lands in
 * xmm0 per the ABI. A double-returning shim's IAT/procaddr thunk jmps here instead
 * of the integer gate (chosen by win32_shim_is_double_ret). Placed in the free
 * region between the dialog trampoline (ends ~0x16A) and the dlgsvc thunk (0x1F0);
 * the 74-byte gate at 0x180 fits with margin. */
#define WIN32_FGATE_VA           0x0000000030000180ULL

/* C9 (GetProcAddress): the loader pre-generates a marshalling thunk for EVERY
 * kernel-side Win32 shim into a fixed region of the shim page, so GetProcAddress
 * can hand back a real callable CPL3 function pointer for any shim resolved by
 * name at runtime. Slot i lives at WIN32_PROCADDR_BASE_VA + i*WIN32_PROCADDR_STRIDE
 * (each slot is the same 10-byte `mov eax,i; jmp gate` thunk an IAT import gets).
 * C18a: the shim count crossed ~300 (SQLite's CRT breadth), so the shim region is
 * now 2 pages (0x2000) and the procaddr table starts at the 2nd page (0x1000).
 * W1 (docs/roadmap-2026-11.md): an import of a shimmed function binds STRAIGHT to
 * that shim's procaddr slot -- one thunk per function, shared by every importer --
 * so the old per-import thunk area (0x200..0xE00) is unused and the procaddr table
 * may fill pages 1-3 (0x1000..0x4000, up to 1228 shims; the bound is checked in
 * pe_loader.c). */
#define WIN32_PROCADDR_BASE_VA   0x0000000030001000ULL
#define WIN32_PROCADDR_STRIDE    10

/* W1: the shim area is 6 pages. Pages 4-5 hold the MISSING-IMPORT stubs: an import
 * no shim implements binds to a 20-byte stub
 *     mov rcx, <user VA of its "DLL!function" text> ; mov eax, <__toby_missing> ;
 *     jmp gate
 * at WIN32_MISSING_STUB_VA + n*20, its text at WIN32_MISSING_NAMES_VA.. . The shim
 * reports the name and ends the process with STATUS_ENTRYPOINT_NOT_FOUND instead
 * of the call jumping to address 0. Stub 0 is the shared fallback for when either
 * area is full. */
#define WIN32_SHIM_PAGES         6u
#define WIN32_MISSING_STUB_VA    0x0000000030004000ULL
#define WIN32_MISSING_STUB_SIZE  20
#define WIN32_MISSING_NAMES_VA   0x0000000030005000ULL
#define WIN32_MISSING_END_VA     0x0000000030006000ULL
#define WIN32_STATUS_ENTRYPOINT_NOT_FOUND 0xC0000139u

/* Resolve "dll!func" to the kernel-side Win32 shim index used to bind an
 * IAT thunk (case-insensitive dll match, exact func match). Returns the
 * index, or -1 if the symbol is not provided. Defined alongside the shim
 * table in src/syscall.c. */
int win32_shim_index(const char *dll, const char *func);

/* Number of entries in the Win32 shim table (src/syscall.c). The loader emits
 * one procaddr thunk per shim; GetProcAddress bounds its index against this. */
int win32_shim_count(void);

/* C20: 1 if shim `idx` returns a `double` (in xmm0) and so must be reached
 * through the float-return gate (WIN32_FGATE_VA) instead of the integer gate.
 * Defined alongside the shim table in src/syscall.c. */
int win32_shim_is_double_ret(int idx);

/* The ABI_SYS_WIN32_DISPATCH target: invoke shim `func_index` with the
 * 8-qword argument array at user VA `args_ptr`. Returns the Win32
 * function's result. Defined in src/syscall.c. */
long win32_dispatch(uint64_t func_index, uint64_t args_ptr);

/* C8 boot-harness observers for the single Win32 GUI window (src/syscall.c).
 * win32_gui_window_fd: live window fd for process `tgid`, or -1 if none yet.
 * win32_gui_fill_color: the window's current FillRect colour (XRGB) so the
 * harness can confirm a click recoloured it. */
int      win32_gui_window_fd(int tgid);
uint32_t win32_gui_fill_color(void);

/* C10 (multi-window): how many live windows process `tgid` owns, and a specific
 * window's current fill colour (XRGB) so the harness can confirm a click
 * recoloured exactly one of several windows. */
int      win32_gui_window_count(int tgid);
uint32_t win32_gui_fill_color_fd(int fd);

/* When on, GetMessage logs each meaningful WM_* it delivers to the WndProc
 * (proof of input delivery, without GUI_TRACE_VERBOSE's per-frame volume). */
void     win32_gui_set_log(bool on);

/* C18c proof: count rendered "ink" pixels in a window region (differing from
 * `bg`) and report the x-centroid of ink in the region's top vs bottom third so
 * the harness can machine-verify bold renders heavier and italic slants. */
int      win32_gui_ink_stats(int fd, int rx, int ry, int rw, int rh, uint32_t bg,
                             int *top_cx, int *bot_cx);

#endif /* TOBYOS_PE_H */
