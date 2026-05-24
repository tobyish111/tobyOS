/* pe_loader.c -- PE/COFF executable loader + Win32 API subset (Phase 9).
 *
 * Provides ability to load and execute Windows PE executables:
 *   1. Parse DOS header, PE signature, COFF header, Optional header
 *   2. Map sections (.text, .data, .rdata, .bss) into process address space
 *   3. Process relocations (for non-preferred base addresses)
 *   4. Resolve imports from the Win32 API shim libraries
 *   5. Set up TLS and exception handling structures
 *   6. Transfer control to the entry point
 *
 * Supported formats:
 *   - PE32+ (64-bit) executables
 *   - PE32 (32-bit) via WoW64-style thunking (future)
 *
 * The Win32 API surface is provided by shim libraries:
 *   - kernel32.dll -> kernel32_shim.c (file I/O, memory, process)
 *   - user32.dll   -> user32_shim.c (windowing, messages)
 *   - gdi32.dll    -> gdi32_shim.c (graphics primitives)
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/vmm.h>
#include <tobyos/proc.h>

/* DOS Header */
struct dos_header {
    uint16_t e_magic;       /* MZ */
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;      /* offset to PE header */
} __attribute__((packed));

/* COFF File Header */
struct coff_header {
    uint16_t machine;
    uint16_t num_sections;
    uint32_t timestamp;
    uint32_t sym_table_ptr;
    uint32_t num_symbols;
    uint16_t opt_header_size;
    uint16_t characteristics;
} __attribute__((packed));

/* PE Optional Header (PE32+) */
struct pe_opt_header64 {
    uint16_t magic;              /* 0x020B for PE32+ */
    uint8_t  major_linker;
    uint8_t  minor_linker;
    uint32_t size_of_code;
    uint32_t size_of_init_data;
    uint32_t size_of_uninit_data;
    uint32_t entry_point_rva;
    uint32_t base_of_code;
    uint64_t image_base;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint16_t major_os_ver;
    uint16_t minor_os_ver;
    uint16_t major_image_ver;
    uint16_t minor_image_ver;
    uint16_t major_subsys_ver;
    uint16_t minor_subsys_ver;
    uint32_t win32_version;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t checksum;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint64_t stack_reserve;
    uint64_t stack_commit;
    uint64_t heap_reserve;
    uint64_t heap_commit;
    uint32_t loader_flags;
    uint32_t num_data_dirs;
} __attribute__((packed));

/* Section Header */
struct pe_section {
    char     name[8];
    uint32_t virtual_size;
    uint32_t virtual_addr;
    uint32_t raw_data_size;
    uint32_t raw_data_ptr;
    uint32_t reloc_ptr;
    uint32_t linenum_ptr;
    uint16_t num_relocs;
    uint16_t num_linenums;
    uint32_t characteristics;
} __attribute__((packed));

/* Data Directory indices */
#define PE_DIR_EXPORT     0
#define PE_DIR_IMPORT     1
#define PE_DIR_RESOURCE   2
#define PE_DIR_EXCEPTION  3
#define PE_DIR_BASERELOC  5
#define PE_DIR_TLS        9
#define PE_DIR_IAT       12

/* Import Directory */
struct pe_import_desc {
    uint32_t original_first_thunk;   /* RVA to INT */
    uint32_t timestamp;
    uint32_t forwarder_chain;
    uint32_t name_rva;               /* RVA to DLL name */
    uint32_t first_thunk;            /* RVA to IAT */
} __attribute__((packed));

/* Base Relocation Block */
struct pe_base_reloc {
    uint32_t page_rva;
    uint32_t block_size;
} __attribute__((packed));

#define PE_RELOC_ABSOLUTE  0
#define PE_RELOC_DIR64    10

/* Machine types */
#define PE_MACHINE_AMD64  0x8664
#define PE_MACHINE_I386   0x014C

/* Section characteristics */
#define PE_SCN_MEM_EXECUTE  0x20000000
#define PE_SCN_MEM_READ     0x40000000
#define PE_SCN_MEM_WRITE    0x80000000

/* ---- Win32 API shim function table ---- */

struct win32_export {
    const char *name;
    uint64_t   addr;
};

/* ---- Registry emulation for advapi32 ---- */

#define PE_REG_MAX_KEYS   256
#define PE_REG_PATH_MAX   128
#define PE_REG_DATA_MAX   256

struct pe_reg_entry {
    bool     active;
    uint32_t hkey_root;
    char     path[PE_REG_PATH_MAX];
    char     name[64];
    uint32_t type;
    uint8_t  data[PE_REG_DATA_MAX];
    uint32_t data_len;
};

static struct pe_reg_entry g_pe_registry[PE_REG_MAX_KEYS];
static int g_pe_reg_count = 0;

/* kernel32 shim functions */
static uint64_t k32_GetLastError(void) { return 0; }
static uint64_t k32_SetLastError(uint64_t code) { (void)code; return 0; }
static uint64_t k32_ExitProcess(uint64_t code) {
    kprintf("[pe] ExitProcess(%lu)\n", (unsigned long)code);
    proc_exit((int)code);
    return 0;
}
static uint64_t k32_GetStdHandle(uint64_t handle) {
    (void)handle;
    return (uint64_t)handle;
}
static uint64_t k32_WriteConsoleA(uint64_t h, uint64_t buf, uint64_t len,
                                   uint64_t written, uint64_t reserved) {
    (void)h; (void)reserved;
    if (buf && len > 0) {
        kprintf("%.*s", (int)len, (const char *)buf);
    }
    if (written) *(uint32_t *)written = (uint32_t)len;
    return 1;
}

/* user32 shim functions */
static uint64_t u32_MessageBoxA(uint64_t hwnd, uint64_t text,
                                uint64_t caption, uint64_t type) {
    (void)hwnd; (void)type;
    kprintf("[pe/user32] MessageBox: \"%s\" - \"%s\"\n",
            caption ? (const char *)caption : "",
            text ? (const char *)text : "");
    return 1; /* IDOK */
}

static uint64_t u32_CreateWindowExA(uint64_t exStyle, uint64_t className,
                                    uint64_t windowName, uint64_t style,
                                    uint64_t x, uint64_t y) {
    (void)exStyle; (void)style; (void)x; (void)y;
    kprintf("[pe/user32] CreateWindowExA: class='%s' title='%s'\n",
            className ? (const char *)className : "",
            windowName ? (const char *)windowName : "");
    return 0x10001;
}

static uint64_t u32_ShowWindow(uint64_t hwnd, uint64_t cmd) {
    kprintf("[pe/user32] ShowWindow(0x%lx, %lu)\n",
            (unsigned long)hwnd, (unsigned long)cmd);
    return 1;
}

static uint64_t u32_UpdateWindow(uint64_t hwnd) {
    kprintf("[pe/user32] UpdateWindow(0x%lx)\n", (unsigned long)hwnd);
    return 1;
}

static uint64_t u32_DefWindowProcA(uint64_t hwnd, uint64_t msg,
                                   uint64_t wparam, uint64_t lparam) {
    (void)hwnd; (void)msg; (void)wparam; (void)lparam;
    return 0;
}

static uint64_t u32_GetMessageA(uint64_t msg, uint64_t hwnd,
                                uint64_t min, uint64_t max) {
    (void)msg; (void)hwnd; (void)min; (void)max;
    kprintf("[pe/user32] GetMessageA (returning 0 = WM_QUIT)\n");
    return 0;
}

static uint64_t u32_DispatchMessageA(uint64_t msg) {
    (void)msg;
    kprintf("[pe/user32] DispatchMessageA\n");
    return 0;
}

static uint64_t u32_PostQuitMessage(uint64_t code) {
    kprintf("[pe/user32] PostQuitMessage(%lu)\n", (unsigned long)code);
    return 0;
}

/* gdi32 shim functions */
static uint64_t gdi_CreateDC(uint64_t driver, uint64_t device,
                             uint64_t output, uint64_t initData) {
    (void)driver; (void)device; (void)output; (void)initData;
    kprintf("[pe/gdi32] CreateDC\n");
    return 0x20001;
}

static uint64_t gdi_GetDC(uint64_t hwnd) {
    (void)hwnd;
    kprintf("[pe/gdi32] GetDC(0x%lx)\n", (unsigned long)hwnd);
    return 0x20002;
}

static uint64_t gdi_ReleaseDC(uint64_t hwnd, uint64_t hdc) {
    (void)hwnd; (void)hdc;
    return 1;
}

static uint64_t gdi_TextOutA(uint64_t hdc, uint64_t x, uint64_t y,
                             uint64_t str, uint64_t len) {
    (void)hdc; (void)x; (void)y;
    if (str && len > 0) {
        kprintf("[pe/gdi32] TextOut(%d,%d): '%.*s'\n",
                (int)x, (int)y, (int)len, (const char *)str);
    }
    return 1;
}

static uint64_t gdi_SetPixel(uint64_t hdc, uint64_t x, uint64_t y,
                             uint64_t color) {
    (void)hdc; (void)x; (void)y; (void)color;
    return (uint64_t)color;
}

static uint64_t gdi_GetPixel(uint64_t hdc, uint64_t x, uint64_t y) {
    (void)hdc; (void)x; (void)y;
    return 0x00FFFFFF;
}

static uint64_t gdi_CreateSolidBrush(uint64_t color) {
    (void)color;
    kprintf("[pe/gdi32] CreateSolidBrush(0x%06lx)\n", (unsigned long)color);
    return 0x30001;
}

static uint64_t gdi_SelectObject(uint64_t hdc, uint64_t obj) {
    (void)hdc; (void)obj;
    return obj;
}

static uint64_t gdi_Rectangle(uint64_t hdc, uint64_t l, uint64_t t,
                              uint64_t r, uint64_t b) {
    (void)hdc;
    kprintf("[pe/gdi32] Rectangle(%d,%d,%d,%d)\n",
            (int)l, (int)t, (int)r, (int)b);
    return 1;
}

static uint64_t gdi_Ellipse(uint64_t hdc, uint64_t l, uint64_t t,
                            uint64_t r, uint64_t b) {
    (void)hdc;
    kprintf("[pe/gdi32] Ellipse(%d,%d,%d,%d)\n",
            (int)l, (int)t, (int)r, (int)b);
    return 1;
}

/* advapi32 shim functions */
static uint64_t adv_RegOpenKeyExA(uint64_t hkey, uint64_t subkey,
                                  uint64_t options, uint64_t sam,
                                  uint64_t result) {
    (void)options; (void)sam;
    kprintf("[pe/advapi32] RegOpenKeyExA(0x%lx, '%s')\n",
            (unsigned long)hkey,
            subkey ? (const char *)subkey : "");
    if (result) *(uint32_t *)result = (uint32_t)(hkey ^ 0x99);
    return 0;
}

static uint64_t adv_RegQueryValueExA(uint64_t hkey, uint64_t name,
                                     uint64_t reserved, uint64_t type,
                                     uint64_t data, uint64_t cbdata) {
    (void)hkey; (void)reserved; (void)type; (void)data; (void)cbdata;
    kprintf("[pe/advapi32] RegQueryValueExA(0x%lx, '%s')\n",
            (unsigned long)hkey,
            name ? (const char *)name : "");
    /* Search our emulated registry */
    for (int i = 0; i < PE_REG_MAX_KEYS; i++) {
        if (!g_pe_registry[i].active) continue;
        if (name && strcmp(g_pe_registry[i].name, (const char *)name) == 0) {
            if (type) *(uint32_t *)type = g_pe_registry[i].type;
            if (data && cbdata) {
                uint32_t cap = *(uint32_t *)cbdata;
                uint32_t copy = g_pe_registry[i].data_len;
                if (copy > cap) copy = cap;
                memcpy((void *)data, g_pe_registry[i].data, copy);
                *(uint32_t *)cbdata = g_pe_registry[i].data_len;
            }
            return 0;
        }
    }
    return 2; /* ERROR_FILE_NOT_FOUND */
}

static uint64_t adv_RegSetValueExA(uint64_t hkey, uint64_t name,
                                   uint64_t reserved, uint64_t type,
                                   uint64_t data, uint64_t cbdata) {
    (void)hkey; (void)reserved;
    kprintf("[pe/advapi32] RegSetValueExA(0x%lx, '%s', type=%lu, %lu bytes)\n",
            (unsigned long)hkey,
            name ? (const char *)name : "",
            (unsigned long)type, (unsigned long)cbdata);
    if (g_pe_reg_count >= PE_REG_MAX_KEYS) return 8; /* ERROR_NOT_ENOUGH_MEMORY */
    for (int i = 0; i < PE_REG_MAX_KEYS; i++) {
        if (!g_pe_registry[i].active) {
            g_pe_registry[i].active = true;
            g_pe_registry[i].hkey_root = (uint32_t)hkey;
            g_pe_registry[i].type = (uint32_t)type;
            if (name) {
                size_t nl = strlen((const char *)name);
                if (nl > 63) nl = 63;
                memcpy(g_pe_registry[i].name, (const char *)name, nl);
                g_pe_registry[i].name[nl] = '\0';
            }
            uint32_t dl = (uint32_t)cbdata;
            if (dl > PE_REG_DATA_MAX) dl = PE_REG_DATA_MAX;
            if (data) memcpy(g_pe_registry[i].data, (const void *)data, dl);
            g_pe_registry[i].data_len = dl;
            g_pe_reg_count++;
            return 0;
        }
    }
    return 8;
}

static uint64_t adv_RegCloseKey(uint64_t hkey) {
    (void)hkey;
    return 0;
}

static const struct win32_export g_kernel32_exports[] = {
    { "GetLastError",     (uint64_t)k32_GetLastError },
    { "SetLastError",     (uint64_t)k32_SetLastError },
    { "ExitProcess",      (uint64_t)k32_ExitProcess },
    { "GetStdHandle",     (uint64_t)k32_GetStdHandle },
    { "WriteConsoleA",    (uint64_t)k32_WriteConsoleA },
    { NULL, 0 }
};

static const struct win32_export g_user32_exports[] = {
    { "MessageBoxA",      (uint64_t)u32_MessageBoxA },
    { "CreateWindowExA",  (uint64_t)u32_CreateWindowExA },
    { "ShowWindow",       (uint64_t)u32_ShowWindow },
    { "UpdateWindow",     (uint64_t)u32_UpdateWindow },
    { "DefWindowProcA",   (uint64_t)u32_DefWindowProcA },
    { "GetMessageA",      (uint64_t)u32_GetMessageA },
    { "DispatchMessageA", (uint64_t)u32_DispatchMessageA },
    { "PostQuitMessage",  (uint64_t)u32_PostQuitMessage },
    { NULL, 0 }
};

static const struct win32_export g_gdi32_exports[] = {
    { "CreateDCA",        (uint64_t)gdi_CreateDC },
    { "GetDC",            (uint64_t)gdi_GetDC },
    { "ReleaseDC",        (uint64_t)gdi_ReleaseDC },
    { "TextOutA",         (uint64_t)gdi_TextOutA },
    { "SetPixel",         (uint64_t)gdi_SetPixel },
    { "GetPixel",         (uint64_t)gdi_GetPixel },
    { "CreateSolidBrush", (uint64_t)gdi_CreateSolidBrush },
    { "SelectObject",     (uint64_t)gdi_SelectObject },
    { "Rectangle",        (uint64_t)gdi_Rectangle },
    { "Ellipse",          (uint64_t)gdi_Ellipse },
    { NULL, 0 }
};

static const struct win32_export g_advapi32_exports[] = {
    { "RegOpenKeyExA",    (uint64_t)adv_RegOpenKeyExA },
    { "RegQueryValueExA", (uint64_t)adv_RegQueryValueExA },
    { "RegSetValueExA",   (uint64_t)adv_RegSetValueExA },
    { "RegCloseKey",      (uint64_t)adv_RegCloseKey },
    { NULL, 0 }
};

static bool dll_match(const char *dll, const char *prefix) {
    if (!dll) return false;
    for (int i = 0; prefix[i]; i++) {
        char c = dll[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        char p = prefix[i];
        if (p >= 'A' && p <= 'Z') p = (char)(p + 32);
        if (c != p) return false;
    }
    return true;
}

static uint64_t resolve_import(const char *dll, const char *func) {
    const struct win32_export *table = NULL;

    if (dll_match(dll, "kernel32"))      table = g_kernel32_exports;
    else if (dll_match(dll, "user32"))   table = g_user32_exports;
    else if (dll_match(dll, "gdi32"))    table = g_gdi32_exports;
    else if (dll_match(dll, "advapi32")) table = g_advapi32_exports;

    if (table) {
        for (int i = 0; table[i].name; i++) {
            if (strcmp(table[i].name, func) == 0)
                return table[i].addr;
        }
    }

    kprintf("[pe] Unresolved import: %s!%s\n", dll ? dll : "?", func);
    return 0;
}

/* ---- PE Loader ---- */

int pe_load(const void *image, size_t size, uint64_t *entry_out) {
    if (!image || size < sizeof(struct dos_header)) return -1;

    const struct dos_header *dos = (const struct dos_header *)image;
    if (dos->e_magic != 0x5A4D) {  /* "MZ" */
        kprintf("[pe] Invalid DOS signature\n");
        return -1;
    }

    if ((uint32_t)dos->e_lfanew + 4 + sizeof(struct coff_header) > size) return -1;

    const uint8_t *pe_sig = (const uint8_t *)image + dos->e_lfanew;
    if (pe_sig[0] != 'P' || pe_sig[1] != 'E' || pe_sig[2] != 0 || pe_sig[3] != 0) {
        kprintf("[pe] Invalid PE signature\n");
        return -1;
    }

    const struct coff_header *coff = (const struct coff_header *)(pe_sig + 4);
    if (coff->machine != PE_MACHINE_AMD64) {
        kprintf("[pe] Unsupported machine type: 0x%04x (need AMD64)\n", coff->machine);
        return -1;
    }

    const struct pe_opt_header64 *opt = (const struct pe_opt_header64 *)(
        (const uint8_t *)coff + sizeof(struct coff_header));
    if (opt->magic != 0x020B) {
        kprintf("[pe] Not a PE32+ image\n");
        return -1;
    }

    uint64_t image_base = opt->image_base;
    uint32_t size_of_image = opt->size_of_image;

    kprintf("[pe] Loading PE64: base=0x%lx size=0x%x entry=0x%x sections=%d\n",
            (unsigned long)image_base, size_of_image,
            opt->entry_point_rva, coff->num_sections);

    /* Map the image into user space via mmap */
    /* For simplicity, allocate at the preferred base if possible */
    long map_addr = sys_mmap(image_base, size_of_image, 7, 0x12, -1, 0);
    if (map_addr < 0) {
        /* Try anywhere */
        map_addr = sys_mmap(0, size_of_image, 7, 0x22, -1, 0);
        if (map_addr < 0) {
            kprintf("[pe] Failed to map image\n");
            return -1;
        }
    }
    uint64_t load_base = (uint64_t)map_addr;
    int64_t delta = (int64_t)(load_base - image_base);

    /* Copy headers */
    memcpy((void *)load_base, image, opt->size_of_headers);

    /* Map sections */
    const struct pe_section *sec = (const struct pe_section *)(
        (const uint8_t *)opt + coff->opt_header_size);
    for (int i = 0; i < coff->num_sections; i++) {
        uint64_t sec_va = load_base + sec[i].virtual_addr;
        if (sec[i].raw_data_size > 0 && sec[i].raw_data_ptr + sec[i].raw_data_size <= size) {
            memcpy((void *)sec_va,
                   (const uint8_t *)image + sec[i].raw_data_ptr,
                   sec[i].raw_data_size);
        }
        /* Zero-fill the remainder */
        if (sec[i].virtual_size > sec[i].raw_data_size) {
            memset((void *)(sec_va + sec[i].raw_data_size), 0,
                   sec[i].virtual_size - sec[i].raw_data_size);
        }
    }

    /* Process base relocations if loaded at non-preferred address */
    if (delta != 0 && opt->num_data_dirs > PE_DIR_BASERELOC) {
        /* Data directories follow the optional header */
        struct { uint32_t rva; uint32_t size; } *dirs =
            (void *)((uint8_t *)opt + sizeof(struct pe_opt_header64));
        uint32_t reloc_rva = dirs[PE_DIR_BASERELOC].rva;
        uint32_t reloc_size = dirs[PE_DIR_BASERELOC].size;

        if (reloc_rva && reloc_size) {
            const uint8_t *reloc_ptr = (const uint8_t *)load_base + reloc_rva;
            const uint8_t *reloc_end = reloc_ptr + reloc_size;

            while (reloc_ptr + sizeof(struct pe_base_reloc) <= reloc_end) {
                const struct pe_base_reloc *blk = (const struct pe_base_reloc *)reloc_ptr;
                if (blk->block_size == 0) break;

                uint32_t num_entries = (blk->block_size - 8) / 2;
                const uint16_t *entries = (const uint16_t *)(reloc_ptr + 8);

                for (uint32_t j = 0; j < num_entries; j++) {
                    uint16_t entry = entries[j];
                    uint8_t type = (entry >> 12) & 0xF;
                    uint16_t offset = entry & 0xFFF;

                    if (type == PE_RELOC_DIR64) {
                        uint64_t *patch = (uint64_t *)(load_base + blk->page_rva + offset);
                        *patch += (uint64_t)delta;
                    }
                }
                reloc_ptr += blk->block_size;
            }
        }
    }

    /* Resolve imports */
    if (opt->num_data_dirs > PE_DIR_IMPORT) {
        struct { uint32_t rva; uint32_t size; } *dirs =
            (void *)((uint8_t *)opt + sizeof(struct pe_opt_header64));
        uint32_t import_rva = dirs[PE_DIR_IMPORT].rva;

        if (import_rva) {
            const struct pe_import_desc *imp =
                (const struct pe_import_desc *)(load_base + import_rva);

            while (imp->name_rva) {
                const char *dll_name = (const char *)(load_base + imp->name_rva);
                uint64_t *thunk = (uint64_t *)(load_base + imp->first_thunk);
                const uint64_t *lookup = imp->original_first_thunk ?
                    (const uint64_t *)(load_base + imp->original_first_thunk) :
                    thunk;

                while (*lookup) {
                    if (!(*lookup & (1ULL << 63))) {
                        /* Import by name */
                        const uint8_t *hint_name = (const uint8_t *)(load_base + (uint32_t)*lookup);
                        const char *func_name = (const char *)(hint_name + 2);
                        *thunk = resolve_import(dll_name, func_name);
                    }
                    lookup++;
                    thunk++;
                }
                imp++;
            }
        }
    }

    *entry_out = load_base + opt->entry_point_rva;
    kprintf("[pe] Image loaded at 0x%lx, entry at 0x%lx\n",
            (unsigned long)load_base, (unsigned long)*entry_out);
    return 0;
}

void pe_loader_init(void) {
    kprintf("[pe] PE/COFF loader initialized (Win32 subsystem)\n");
}
