/* ld/main.c -- Minimal ELF linker for tobyOS.
 *
 * Reads multiple ELF .o object files, merges .text/.data/.bss sections,
 * resolves symbol references, applies relocations, and outputs an ELF
 * executable at base address 0x400000 (matching program.ld layout).
 *
 * Options: -o output, -l library, -L library_path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_INPUTS   32
#define MAX_SECTIONS 64
#define MAX_SYMS     2048
#define MAX_RELOCS   4096
#define MAX_CODE     (512*1024)
#define MAX_DATA     (128*1024)

#define BASE_ADDR    0x400000

/* ---- ELF structures ------------------------------------------------- */

#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define ET_REL       1
#define ET_EXEC      2
#define EM_X86_64    62
#define PT_LOAD      1
#define PF_R         4
#define PF_W         2
#define PF_X         1
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHF_ALLOC    2
#define SHF_EXEC     4
#define SHF_WRITE    1
#define STB_GLOBAL   1
#define STB_LOCAL    0
#define R_X86_64_64      1
#define R_X86_64_PC32    2
#define R_X86_64_PLT32   4
#define SHN_UNDEF    0

typedef struct {
    unsigned char e_ident[16];
    unsigned short e_type, e_machine;
    unsigned int e_version;
    unsigned long e_entry, e_phoff, e_shoff;
    unsigned int e_flags;
    unsigned short e_ehsize, e_phentsize, e_phnum;
    unsigned short e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    unsigned int sh_name, sh_type;
    unsigned long sh_flags, sh_addr, sh_offset, sh_size;
    unsigned int sh_link, sh_info;
    unsigned long sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct {
    unsigned int st_name;
    unsigned char st_info, st_other;
    unsigned short st_shndx;
    unsigned long st_value, st_size;
} Elf64_Sym;

typedef struct {
    unsigned long r_offset;
    unsigned long r_info;
    long r_addend;
} Elf64_Rela;

typedef struct {
    unsigned int p_type, p_flags;
    unsigned long p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

/* ---- Linker state --------------------------------------------------- */

typedef struct {
    char name[128];
    unsigned long value;
    int defined;
    int section;
    int input_file;
} LinkSymbol;

typedef struct {
    int input_file;
    int section_idx;
    unsigned long offset;
    unsigned long sym_info;
    long addend;
    int type;
    char sym_name[128];
} LinkReloc;

typedef struct {
    unsigned char text[MAX_CODE];
    int text_len;
    unsigned char data_buf[MAX_DATA];
    int data_len;
    int bss_len;

    LinkSymbol syms[MAX_SYMS];
    int nsyms;

    LinkReloc relocs[MAX_RELOCS];
    int nrelocs;

    const char *outfile;
    const char *entry_name;
    int had_error;
} LinkState;

static void ld_error(LinkState *ld, const char *msg) {
    fprintf(stderr, "ld: %s\n", msg);
    ld->had_error = 1;
}

static LinkSymbol *find_sym(LinkState *ld, const char *name) {
    for (int i = 0; i < ld->nsyms; i++)
        if (strcmp(ld->syms[i].name, name) == 0) return &ld->syms[i];
    return NULL;
}

static LinkSymbol *add_sym(LinkState *ld, const char *name) {
    LinkSymbol *s = find_sym(ld, name);
    if (s) return s;
    if (ld->nsyms >= MAX_SYMS) { ld_error(ld, "too many symbols"); return NULL; }
    s = &ld->syms[ld->nsyms++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 127);
    return s;
}

/* ---- Read ELF object ------------------------------------------------ */

static unsigned short read_le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned int read_le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}
static unsigned long read_le64(const unsigned char *p) {
    return (unsigned long)read_le32(p) | ((unsigned long)read_le32(p + 4) << 32);
}

static int load_object(LinkState *ld, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "ld: cannot open %s\n", path); return -1; }

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(fsz);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, fsz, f);
    fclose(f);

    /* Validate ELF header */
    if (fsz < (long)sizeof(Elf64_Ehdr) ||
        buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        fprintf(stderr, "ld: %s: not an ELF file\n", path);
        free(buf);
        return -1;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    if (read_le16((unsigned char*)&ehdr->e_type) != ET_REL) {
        fprintf(stderr, "ld: %s: not a relocatable object\n", path);
        free(buf);
        return -1;
    }

    int shnum = read_le16((unsigned char*)&ehdr->e_shnum);
    unsigned long shoff = read_le64((unsigned char*)&ehdr->e_shoff);
    int shstrndx = read_le16((unsigned char*)&ehdr->e_shstrndx);

    /* Read section headers */
    Elf64_Shdr *shdrs = (Elf64_Shdr *)(buf + shoff);
    const char *shstrtab = NULL;
    if (shstrndx < shnum) {
        Elf64_Shdr *sh = &shdrs[shstrndx];
        shstrtab = (const char *)(buf + read_le64((unsigned char*)&sh->sh_offset));
    }

    /* Track text/data offsets for this file */
    int text_base_off = ld->text_len;
    int data_base_off = ld->data_len;

    int sec_offsets[MAX_SECTIONS];
    memset(sec_offsets, 0, sizeof(sec_offsets));

    /* First pass: collect .text, .data, .bss */
    for (int i = 0; i < shnum && i < MAX_SECTIONS; i++) {
        Elf64_Shdr *sh = &shdrs[i];
        unsigned int sh_type = read_le32((unsigned char*)&sh->sh_type);
        unsigned long sh_flags = read_le64((unsigned char*)&sh->sh_flags);
        unsigned long sh_size = read_le64((unsigned char*)&sh->sh_size);
        unsigned long sh_offset = read_le64((unsigned char*)&sh->sh_offset);
        unsigned int sh_name = read_le32((unsigned char*)&sh->sh_name);
        const char *sname = shstrtab ? shstrtab + sh_name : "";

        if (sh_type == SHT_PROGBITS && (sh_flags & SHF_EXEC)) {
            sec_offsets[i] = text_base_off;
            if (ld->text_len + (int)sh_size <= MAX_CODE) {
                memcpy(ld->text + ld->text_len, buf + sh_offset, sh_size);
                ld->text_len += (int)sh_size;
            }
        } else if (sh_type == SHT_PROGBITS && (sh_flags & SHF_WRITE)) {
            sec_offsets[i] = data_base_off;
            if (ld->data_len + (int)sh_size <= MAX_DATA) {
                memcpy(ld->data_buf + ld->data_len, buf + sh_offset, sh_size);
                ld->data_len += (int)sh_size;
            }
        } else if (sh_type == SHT_PROGBITS && (sh_flags & SHF_ALLOC)) {
            /* Read-only data -- put in data section */
            sec_offsets[i] = data_base_off;
            if (ld->data_len + (int)sh_size <= MAX_DATA) {
                memcpy(ld->data_buf + ld->data_len, buf + sh_offset, sh_size);
                ld->data_len += (int)sh_size;
            }
        } else if (sh_type == SHT_NOBITS) {
            sec_offsets[i] = ld->bss_len;
            ld->bss_len += (int)sh_size;
        }
        (void)sname;
    }

    /* Second pass: collect symbols */
    for (int i = 0; i < shnum; i++) {
        Elf64_Shdr *sh = &shdrs[i];
        if (read_le32((unsigned char*)&sh->sh_type) != SHT_SYMTAB) continue;

        unsigned long sym_off = read_le64((unsigned char*)&sh->sh_offset);
        unsigned long sym_size = read_le64((unsigned char*)&sh->sh_size);
        int sym_count = (int)(sym_size / sizeof(Elf64_Sym));
        unsigned int strtab_idx = read_le32((unsigned char*)&sh->sh_link);

        const char *strtab = NULL;
        if (strtab_idx < (unsigned)shnum) {
            Elf64_Shdr *stsh = &shdrs[strtab_idx];
            strtab = (const char *)(buf + read_le64((unsigned char*)&stsh->sh_offset));
        }

        Elf64_Sym *syms = (Elf64_Sym *)(buf + sym_off);
        for (int j = 0; j < sym_count; j++) {
            Elf64_Sym *sym = &syms[j];
            unsigned int st_name = read_le32((unsigned char*)&sym->st_name);
            unsigned char st_info = sym->st_info;
            unsigned short st_shndx = read_le16((unsigned char*)&sym->st_shndx);
            unsigned long st_value = read_le64((unsigned char*)&sym->st_value);

            if (st_name == 0) continue;
            const char *name = strtab ? strtab + st_name : "";
            if (name[0] == '\0') continue;

            int binding = st_info >> 4;
            if (binding == STB_LOCAL) continue;

            LinkSymbol *ls = add_sym(ld, name);
            if (ls && st_shndx != SHN_UNDEF && !ls->defined) {
                ls->defined = 1;
                ls->section = st_shndx;
                ls->value = sec_offsets[st_shndx] + st_value;
            }
        }

        /* Collect relocations */
        for (int ri = 0; ri < shnum; ri++) {
            Elf64_Shdr *rsh = &shdrs[ri];
            if (read_le32((unsigned char*)&rsh->sh_type) != SHT_RELA) continue;
            unsigned int rsh_info = read_le32((unsigned char*)&rsh->sh_info);

            unsigned long rela_off = read_le64((unsigned char*)&rsh->sh_offset);
            unsigned long rela_size = read_le64((unsigned char*)&rsh->sh_size);
            int rela_count = (int)(rela_size / sizeof(Elf64_Rela));

            Elf64_Rela *relas = (Elf64_Rela *)(buf + rela_off);
            for (int k = 0; k < rela_count; k++) {
                Elf64_Rela *r = &relas[k];
                unsigned long r_off = read_le64((unsigned char*)&r->r_offset);
                unsigned long r_info = read_le64((unsigned char*)&r->r_info);
                long r_addend;
                memcpy(&r_addend, &r->r_addend, 8);

                int sym_idx = (int)(r_info >> 32);
                int rtype = (int)(r_info & 0xFFFFFFFF);

                const char *sym_name = "";
                if (sym_idx > 0 && sym_idx < sym_count) {
                    unsigned int sn = read_le32((unsigned char*)&syms[sym_idx].st_name);
                    if (strtab && sn > 0) sym_name = strtab + sn;
                }

                if (ld->nrelocs < MAX_RELOCS) {
                    LinkReloc *lr = &ld->relocs[ld->nrelocs++];
                    lr->offset = sec_offsets[rsh_info] + r_off;
                    lr->type = rtype;
                    lr->addend = r_addend;
                    strncpy(lr->sym_name, sym_name, 127);
                    lr->section_idx = rsh_info;
                }
            }
        }
    }

    free(buf);
    return 0;
}

/* ---- Write ELF executable ------------------------------------------- */

static void write_le32_buf(unsigned char *p, unsigned int v) {
    p[0] = v; p[1] = v>>8; p[2] = v>>16; p[3] = v>>24;
}

static int write_executable(LinkState *ld) {
    FILE *f = fopen(ld->outfile, "w");
    if (!f) { fprintf(stderr, "ld: cannot open %s\n", ld->outfile); return -1; }

    unsigned long hdr_size = sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr);
    unsigned long text_vaddr = BASE_ADDR + hdr_size;
    unsigned long text_size = ld->text_len;
    unsigned long data_vaddr = (text_vaddr + text_size + 0xFFF) & ~0xFFFUL;
    unsigned long data_file_off = (hdr_size + text_size + 0xFFF) & ~0xFFFUL;

    /* Apply relocations */
    for (int i = 0; i < ld->nrelocs; i++) {
        LinkReloc *r = &ld->relocs[i];
        LinkSymbol *sym = find_sym(ld, r->sym_name);
        if (!sym || !sym->defined) {
            fprintf(stderr, "ld: undefined symbol: %s\n", r->sym_name);
            ld->had_error = 1;
            continue;
        }

        /* Determine symbol address based on section type */
        unsigned long sym_addr;
        Elf64_Shdr *dummy = NULL;
        (void)dummy;

        /* Simple heuristic: text symbols get text_vaddr base, data get data_vaddr */
        if (sym->value < (unsigned long)ld->text_len)
            sym_addr = text_vaddr + sym->value;
        else
            sym_addr = data_vaddr + (sym->value - ld->text_len);

        unsigned long reloc_addr = text_vaddr + r->offset;
        unsigned char *patch = ld->text + r->offset;

        switch (r->type) {
        case R_X86_64_PC32:
        case R_X86_64_PLT32: {
            int rel = (int)((long)sym_addr - (long)reloc_addr + r->addend);
            write_le32_buf(patch, (unsigned int)rel);
            break;
        }
        case R_X86_64_64: {
            unsigned long abs_val = sym_addr + r->addend;
            for (int j = 0; j < 8; j++) patch[j] = (abs_val >> (j*8)) & 0xFF;
            break;
        }
        }
    }

    /* Find entry point */
    unsigned long entry = text_vaddr;
    LinkSymbol *entry_sym = find_sym(ld, ld->entry_name);
    if (!entry_sym) entry_sym = find_sym(ld, "_start");
    if (!entry_sym) entry_sym = find_sym(ld, "main");
    if (entry_sym && entry_sym->defined) {
        if (entry_sym->value < (unsigned long)ld->text_len)
            entry = text_vaddr + entry_sym->value;
    }

    /* ELF header */
    Elf64_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = 0x7F; ehdr.e_ident[1] = 'E';
    ehdr.e_ident[2] = 'L';  ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = ELFCLASS64; ehdr.e_ident[5] = ELFDATA2LSB;
    ehdr.e_ident[6] = 1;
    ehdr.e_type = ET_EXEC;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = 1;
    ehdr.e_entry = entry;
    ehdr.e_phoff = sizeof(Elf64_Ehdr);
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum = 2;

    /* Text segment phdr */
    Elf64_Phdr text_phdr;
    memset(&text_phdr, 0, sizeof(text_phdr));
    text_phdr.p_type = PT_LOAD;
    text_phdr.p_flags = PF_R | PF_X;
    text_phdr.p_offset = 0;
    text_phdr.p_vaddr = BASE_ADDR;
    text_phdr.p_paddr = BASE_ADDR;
    text_phdr.p_filesz = hdr_size + text_size;
    text_phdr.p_memsz = hdr_size + text_size;
    text_phdr.p_align = 0x1000;

    /* Data segment phdr */
    Elf64_Phdr data_phdr;
    memset(&data_phdr, 0, sizeof(data_phdr));
    data_phdr.p_type = PT_LOAD;
    data_phdr.p_flags = PF_R | PF_W;
    data_phdr.p_offset = data_file_off;
    data_phdr.p_vaddr = data_vaddr;
    data_phdr.p_paddr = data_vaddr;
    data_phdr.p_filesz = ld->data_len;
    data_phdr.p_memsz = ld->data_len + ld->bss_len;
    data_phdr.p_align = 0x1000;

    fwrite(&ehdr, sizeof(ehdr), 1, f);
    fwrite(&text_phdr, sizeof(text_phdr), 1, f);
    fwrite(&data_phdr, sizeof(data_phdr), 1, f);
    fwrite(ld->text, 1, ld->text_len, f);

    /* Pad to data offset */
    unsigned long cur = hdr_size + text_size;
    while (cur < data_file_off) { fputc(0, f); cur++; }

    fwrite(ld->data_buf, 1, ld->data_len, f);

    fclose(f);
    return 0;
}

/* ---- Main ----------------------------------------------------------- */

int main(int argc, char **argv) {
    LinkState state;
    memset(&state, 0, sizeof(state));
    LinkState *ld = &state;
    ld->outfile = "a.out";
    ld->entry_name = "_start";

    const char *inputs[MAX_INPUTS];
    int ninputs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            ld->outfile = argv[++i];
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            ld->entry_name = argv[++i];
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            i++; /* Library path -- stub */
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            i++; /* Library search -- stub */
        } else if (strncmp(argv[i], "-L", 2) == 0) {
            /* -Lpath -- stub */
        } else if (strncmp(argv[i], "-l", 2) == 0) {
            /* -llib -- stub */
        } else if (argv[i][0] != '-') {
            if (ninputs < MAX_INPUTS) inputs[ninputs++] = argv[i];
        }
    }

    if (ninputs == 0) {
        fprintf(stderr, "Usage: ld [-o output] [-e entry] input.o ...\n");
        return 1;
    }

    for (int i = 0; i < ninputs; i++) {
        if (load_object(ld, inputs[i]) != 0) {
            ld->had_error = 1;
        }
    }

    if (ld->had_error) return 1;

    int rc = write_executable(ld);
    if (rc == 0 && !ld->had_error) {
        printf("ld: wrote %s (%d bytes .text, %d bytes .data, %d bytes .bss)\n",
               ld->outfile, ld->text_len, ld->data_len, ld->bss_len);
    }
    return ld->had_error ? 1 : rc;
}
