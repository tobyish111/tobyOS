/* as/main.c -- Minimal x86_64 assembler for tobyOS.
 *
 * Parses Intel-syntax assembly and outputs ELF .o object files.
 * Supports: mov, add, sub, mul, div, push, pop, call, ret, jmp, jcc,
 *           cmp, test, xor, and, or, shl, shr, lea, nop, syscall, int.
 * Registers: rax-r15, eax-r15d, al-r15b.
 * Addressing: [reg], [reg+disp], [reg+reg*scale+disp].
 * Sections: .text, .data, .bss. Labels, .global, .extern directives.
 * Output: ELF .o with relocations (R_X86_64_PC32, R_X86_64_PLT32, R_X86_64_64).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define MAX_CODE    (64*1024)
#define MAX_DATA    (16*1024)
#define MAX_BSS     (16*1024)
#define MAX_SYMS    512
#define MAX_RELOCS  1024
#define MAX_LINE    1024
#define MAX_LABELS  512

/* ---- ELF definitions ------------------------------------------------ */

#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_REL      1
#define EM_X86_64   62
#define SHT_NULL    0
#define SHT_PROGBITS 1
#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define SHT_RELA    4
#define SHT_NOBITS  8
#define SHF_WRITE   1
#define SHF_ALLOC   2
#define SHF_EXEC    4
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STT_NOTYPE  0
#define STT_FUNC    2
#define STT_SECTION 3
#define R_X86_64_64     1
#define R_X86_64_PC32   2
#define R_X86_64_PLT32  4

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

/* ---- Assembler state ------------------------------------------------ */

enum section_id { SEC_TEXT, SEC_DATA, SEC_BSS };

typedef struct {
    char name[64];
    int section;
    int offset;
    int is_global;
    int is_extern;
    int defined;
} AsmSymbol;

typedef struct {
    int section;
    int offset;
    char sym_name[64];
    int type;
    int addend;
} AsmReloc;

typedef struct {
    unsigned char text[MAX_CODE];
    int text_len;
    unsigned char data[MAX_DATA];
    int data_len;
    int bss_len;

    AsmSymbol syms[MAX_SYMS];
    int nsyms;

    AsmReloc relocs[MAX_RELOCS];
    int nrelocs;

    int cur_section;
    int line;
    const char *outfile;
    int had_error;
} AsmState;

static void asm_error(AsmState *a, const char *msg) {
    fprintf(stderr, "as: line %d: %s\n", a->line, msg);
    a->had_error = 1;
}

/* ---- Symbol management ---------------------------------------------- */

static AsmSymbol *find_symbol(AsmState *a, const char *name) {
    for (int i = 0; i < a->nsyms; i++)
        if (strcmp(a->syms[i].name, name) == 0) return &a->syms[i];
    return NULL;
}

static AsmSymbol *add_symbol(AsmState *a, const char *name) {
    AsmSymbol *s = find_symbol(a, name);
    if (s) return s;
    if (a->nsyms >= MAX_SYMS) { asm_error(a, "too many symbols"); return NULL; }
    s = &a->syms[a->nsyms++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 63);
    return s;
}

static void emit_text(AsmState *a, unsigned char b) {
    if (a->text_len < MAX_CODE) a->text[a->text_len++] = b;
}

static void emit_text32(AsmState *a, int v) {
    emit_text(a, v & 0xFF);
    emit_text(a, (v >> 8) & 0xFF);
    emit_text(a, (v >> 16) & 0xFF);
    emit_text(a, (v >> 24) & 0xFF);
}

static void emit_text64(AsmState *a, long v) {
    emit_text32(a, (int)(v & 0xFFFFFFFF));
    emit_text32(a, (int)((v >> 32) & 0xFFFFFFFF));
}

static void emit_data_byte(AsmState *a, unsigned char b) {
    if (a->data_len < MAX_DATA) a->data[a->data_len++] = b;
}

static void add_reloc(AsmState *a, int sec, int off, const char *sym, int type, int addend) {
    if (a->nrelocs >= MAX_RELOCS) return;
    AsmReloc *r = &a->relocs[a->nrelocs++];
    r->section = sec;
    r->offset = off;
    strncpy(r->sym_name, sym, 63);
    r->type = type;
    r->addend = addend;
}

/* ---- Register parsing ----------------------------------------------- */

struct reg_info {
    const char *name;
    int code;
    int size;
    int rex_ext;
};

static const struct reg_info regs[] = {
    {"rax",0,8,0},{"rcx",1,8,0},{"rdx",2,8,0},{"rbx",3,8,0},
    {"rsp",4,8,0},{"rbp",5,8,0},{"rsi",6,8,0},{"rdi",7,8,0},
    {"r8",0,8,1},{"r9",1,8,1},{"r10",2,8,1},{"r11",3,8,1},
    {"r12",4,8,1},{"r13",5,8,1},{"r14",6,8,1},{"r15",7,8,1},
    {"eax",0,4,0},{"ecx",1,4,0},{"edx",2,4,0},{"ebx",3,4,0},
    {"esp",4,4,0},{"ebp",5,4,0},{"esi",6,4,0},{"edi",7,4,0},
    {"r8d",0,4,1},{"r9d",1,4,1},{"r10d",2,4,1},{"r11d",3,4,1},
    {"r12d",4,4,1},{"r13d",5,4,1},{"r14d",6,4,1},{"r15d",7,4,1},
    {"al",0,1,0},{"cl",1,1,0},{"dl",2,1,0},{"bl",3,1,0},
    {"spl",4,1,0},{"bpl",5,1,0},{"sil",6,1,0},{"dil",7,1,0},
    {"r8b",0,1,1},{"r9b",1,1,1},{"r10b",2,1,1},{"r11b",3,1,1},
    {"r12b",4,1,1},{"r13b",5,1,1},{"r14b",6,1,1},{"r15b",7,1,1},
    {NULL,0,0,0}
};

static const struct reg_info *find_reg(const char *name) {
    for (int i = 0; regs[i].name; i++)
        if (strcmp(name, regs[i].name) == 0) return &regs[i];
    return NULL;
}

/* ---- Operand types -------------------------------------------------- */

enum op_type { OP_NONE, OP_REG, OP_IMM, OP_MEM, OP_LABEL };

typedef struct {
    int type;
    const struct reg_info *reg;
    long imm;
    const struct reg_info *base;
    const struct reg_info *index;
    int scale;
    int disp;
    char label[64];
} Operand;

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

static long parse_number(const char **p) {
    long val = 0;
    int neg = 0;
    if (**p == '-') { neg = 1; (*p)++; }
    if (**p == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        *p += 2;
        while (isxdigit(**p)) {
            int d = isdigit(**p) ? **p - '0' : (tolower(**p) - 'a' + 10);
            val = val * 16 + d;
            (*p)++;
        }
    } else {
        while (isdigit(**p)) { val = val * 10 + (**p - '0'); (*p)++; }
    }
    return neg ? -val : val;
}

static int parse_operand(AsmState *a, const char **p, Operand *op) {
    (void)a;
    skip_ws(p);
    memset(op, 0, sizeof(*op));

    if (!**p || **p == '\n' || **p == ';' || **p == '#') {
        op->type = OP_NONE; return 0;
    }

    /* Memory operand: [base + index*scale + disp] */
    if (**p == '[') {
        (*p)++;
        op->type = OP_MEM;
        skip_ws(p);
        /* Parse base register */
        char name[32]; int ni = 0;
        while (isalnum(**p) || **p == '_') { if (ni < 31) name[ni++] = **p; (*p)++; }
        name[ni] = '\0';
        op->base = find_reg(name);

        skip_ws(p);
        if (**p == '+' || **p == '-') {
            int sign = (**p == '-') ? -1 : 1;
            (*p)++;
            skip_ws(p);
            ni = 0;
            while (isalnum(**p) || **p == '_') { if (ni < 31) name[ni++] = **p; (*p)++; }
            name[ni] = '\0';
            const struct reg_info *r2 = find_reg(name);
            if (r2) {
                skip_ws(p);
                if (**p == '*') {
                    (*p)++;
                    skip_ws(p);
                    op->scale = (int)parse_number(p);
                    op->index = r2;
                } else {
                    op->index = r2;
                    op->scale = 1;
                }
                skip_ws(p);
                if (**p == '+' || **p == '-') {
                    int s2 = (**p == '-') ? -1 : 1;
                    (*p)++;
                    skip_ws(p);
                    op->disp = (int)(s2 * parse_number(p));
                }
            } else {
                char *end;
                long v = strtol(name, &end, 0);
                if (*end == '\0') op->disp = (int)(sign * v);
                else op->disp = 0;
                skip_ws(p);
                if (**p == '+' || **p == '-') {
                    int s2 = (**p == '-') ? -1 : 1;
                    (*p)++;
                    skip_ws(p);
                    op->disp += (int)(s2 * parse_number(p));
                }
            }
        }
        skip_ws(p);
        if (**p == ']') (*p)++;
        return 1;
    }

    /* Register or label? */
    if (isalpha(**p) || **p == '_' || **p == '.') {
        char name[64]; int ni = 0;
        while (isalnum(**p) || **p == '_' || **p == '.') {
            if (ni < 63) name[ni++] = **p;
            (*p)++;
        }
        name[ni] = '\0';

        const struct reg_info *r = find_reg(name);
        if (r) {
            op->type = OP_REG;
            op->reg = r;
            return 1;
        }
        op->type = OP_LABEL;
        strncpy(op->label, name, 63);
        return 1;
    }

    /* Immediate */
    if (isdigit(**p) || **p == '-' || **p == '0') {
        op->type = OP_IMM;
        op->imm = parse_number(p);
        return 1;
    }

    return 0;
}

/* ---- Instruction encoding ------------------------------------------- */

static unsigned char modrm(int mod, int reg, int rm) {
    return (unsigned char)((mod << 6) | (reg << 3) | rm);
}

static unsigned char sib_byte(int scale, int index, int base) {
    int s = 0;
    if (scale == 2) s = 1;
    else if (scale == 4) s = 2;
    else if (scale == 8) s = 3;
    return (unsigned char)((s << 6) | (index << 3) | base);
}

static void emit_rex(AsmState *a, int w, int r_ext, int x_ext, int b_ext) {
    unsigned char rex = 0x40;
    if (w) rex |= 0x08;
    if (r_ext) rex |= 0x04;
    if (x_ext) rex |= 0x02;
    if (b_ext) rex |= 0x01;
    if (rex != 0x40 || w) emit_text(a, rex);
}

static void encode_mem(AsmState *a, int reg_code, Operand *mem) {
    if (!mem->base && !mem->index) {
        emit_text(a, modrm(0, reg_code, 4));
        emit_text(a, sib_byte(1, 4, 5));
        emit_text32(a, mem->disp);
        return;
    }

    int base_code = mem->base ? mem->base->code : 5;
    if (mem->index) {
        int mod = 0;
        if (mem->disp != 0 || base_code == 5) {
            mod = (mem->disp >= -128 && mem->disp <= 127) ? 1 : 2;
        }
        emit_text(a, modrm(mod, reg_code, 4));
        emit_text(a, sib_byte(mem->scale ? mem->scale : 1, mem->index->code, base_code));
        if (mod == 1) emit_text(a, (unsigned char)mem->disp);
        else if (mod == 2 || base_code == 5) emit_text32(a, mem->disp);
    } else {
        if (mem->disp == 0 && base_code != 5) {
            emit_text(a, modrm(0, reg_code, base_code));
            if (base_code == 4) emit_text(a, 0x24);
        } else if (mem->disp >= -128 && mem->disp <= 127) {
            emit_text(a, modrm(1, reg_code, base_code));
            if (base_code == 4) emit_text(a, 0x24);
            emit_text(a, (unsigned char)mem->disp);
        } else {
            emit_text(a, modrm(2, reg_code, base_code));
            if (base_code == 4) emit_text(a, 0x24);
            emit_text32(a, mem->disp);
        }
    }
}

/* ---- Instruction table ---------------------------------------------- */

typedef struct {
    const char *mnemonic;
    int opcode_base;
    int modrm_ext;
    int flags;
} InsnEntry;

#define F_NONE    0
#define F_NOREX   1

static void assemble_line(AsmState *a, const char *line) {
    const char *p = line;
    skip_ws(&p);

    if (!*p || *p == ';' || *p == '#' || *p == '\n') return;

    /* Directives */
    if (*p == '.') {
        p++;
        if (strncmp(p, "text", 4) == 0) { a->cur_section = SEC_TEXT; return; }
        if (strncmp(p, "data", 4) == 0) { a->cur_section = SEC_DATA; return; }
        if (strncmp(p, "bss", 3) == 0)  { a->cur_section = SEC_BSS; return; }
        if (strncmp(p, "global", 6) == 0 || strncmp(p, "globl", 5) == 0) {
            p += (p[4] == 'l') ? 5 : 6;
            skip_ws(&p);
            char name[64]; int ni = 0;
            while (isalnum(*p) || *p == '_') { if (ni < 63) name[ni++] = *p; p++; }
            name[ni] = '\0';
            AsmSymbol *s = add_symbol(a, name);
            if (s) s->is_global = 1;
            return;
        }
        if (strncmp(p, "extern", 6) == 0) {
            p += 6; skip_ws(&p);
            char name[64]; int ni = 0;
            while (isalnum(*p) || *p == '_') { if (ni < 63) name[ni++] = *p; p++; }
            name[ni] = '\0';
            AsmSymbol *s = add_symbol(a, name);
            if (s) { s->is_extern = 1; s->defined = 0; }
            return;
        }
        if (strncmp(p, "byte", 4) == 0) {
            p += 4; skip_ws(&p);
            while (isdigit(*p) || *p == '0') {
                long v = parse_number(&p);
                emit_data_byte(a, (unsigned char)v);
                skip_ws(&p);
                if (*p == ',') { p++; skip_ws(&p); }
            }
            return;
        }
        if (strncmp(p, "quad", 4) == 0 || strncmp(p, "long", 4) == 0) {
            int is_quad = (p[0] == 'q');
            p += 4; skip_ws(&p);
            long v = parse_number(&p);
            if (is_quad) {
                for (int i = 0; i < 8; i++) emit_data_byte(a, (v >> (i*8)) & 0xFF);
            } else {
                for (int i = 0; i < 4; i++) emit_data_byte(a, (v >> (i*8)) & 0xFF);
            }
            return;
        }
        if (strncmp(p, "zero", 4) == 0 || strncmp(p, "skip", 4) == 0) {
            p += 4; skip_ws(&p);
            int n = (int)parse_number(&p);
            for (int i = 0; i < n; i++) emit_data_byte(a, 0);
            return;
        }
        if (strncmp(p, "ascii", 5) == 0) {
            int with_zero = (p[5] == 'z');
            p += with_zero ? 6 : 5;
            skip_ws(&p);
            if (*p == '"') { p++;
                while (*p && *p != '"') {
                    if (*p == '\\') {
                        p++;
                        if (*p == 'n') emit_data_byte(a, '\n');
                        else if (*p == 't') emit_data_byte(a, '\t');
                        else if (*p == '0') emit_data_byte(a, 0);
                        else emit_data_byte(a, *p);
                    } else emit_data_byte(a, *p);
                    p++;
                }
                if (with_zero) emit_data_byte(a, 0);
            }
            return;
        }
        return;
    }

    /* Label */
    {
        const char *colon = strchr(p, ':');
        if (colon) {
            char name[64]; int ni = 0;
            const char *q = p;
            while (q < colon && ni < 63) name[ni++] = *q++;
            name[ni] = '\0';
            while (ni > 0 && name[ni-1] == ' ') name[--ni] = '\0';

            AsmSymbol *s = add_symbol(a, name);
            if (s) {
                s->section = a->cur_section;
                s->offset = (a->cur_section == SEC_TEXT) ? a->text_len :
                            (a->cur_section == SEC_DATA) ? a->data_len : a->bss_len;
                s->defined = 1;
            }
            p = colon + 1;
            skip_ws(&p);
            if (!*p || *p == ';' || *p == '#' || *p == '\n') return;
        }
    }

    /* Parse mnemonic */
    char mnem[32]; int mi = 0;
    while (isalpha(*p) && mi < 31) mnem[mi++] = tolower(*p++);
    mnem[mi] = '\0';

    skip_ws(&p);
    Operand op1, op2;
    memset(&op1, 0, sizeof(op1));
    memset(&op2, 0, sizeof(op2));
    parse_operand(a, &p, &op1);
    skip_ws(&p);
    if (*p == ',') { p++; parse_operand(a, &p, &op2); }

    /* Encode instructions */
    if (strcmp(mnem, "nop") == 0) { emit_text(a, 0x90); return; }
    if (strcmp(mnem, "ret") == 0) { emit_text(a, 0xC3); return; }
    if (strcmp(mnem, "syscall") == 0) { emit_text(a, 0x0F); emit_text(a, 0x05); return; }
    if (strcmp(mnem, "int") == 0 && op1.type == OP_IMM) {
        emit_text(a, 0xCD); emit_text(a, (unsigned char)op1.imm); return;
    }
    if (strcmp(mnem, "push") == 0 && op1.type == OP_REG && op1.reg->size == 8) {
        if (op1.reg->rex_ext) emit_text(a, 0x41);
        emit_text(a, 0x50 + op1.reg->code);
        return;
    }
    if (strcmp(mnem, "pop") == 0 && op1.type == OP_REG && op1.reg->size == 8) {
        if (op1.reg->rex_ext) emit_text(a, 0x41);
        emit_text(a, 0x58 + op1.reg->code);
        return;
    }

    /* mov reg, imm */
    if (strcmp(mnem, "mov") == 0) {
        if (op1.type == OP_REG && op2.type == OP_IMM) {
            emit_rex(a, op1.reg->size == 8, 0, 0, op1.reg->rex_ext);
            if (op1.reg->size == 8) {
                emit_text(a, 0xB8 + op1.reg->code);
                emit_text64(a, op2.imm);
            } else {
                emit_text(a, 0xB8 + op1.reg->code);
                emit_text32(a, (int)op2.imm);
            }
            return;
        }
        if (op1.type == OP_REG && op2.type == OP_REG) {
            emit_rex(a, op1.reg->size == 8, op2.reg->rex_ext, 0, op1.reg->rex_ext);
            emit_text(a, 0x89);
            emit_text(a, modrm(3, op2.reg->code, op1.reg->code));
            return;
        }
        if (op1.type == OP_REG && op2.type == OP_MEM) {
            emit_rex(a, op1.reg->size == 8, op1.reg->rex_ext, 0,
                     op2.base ? op2.base->rex_ext : 0);
            emit_text(a, 0x8B);
            encode_mem(a, op1.reg->code, &op2);
            return;
        }
        if (op1.type == OP_MEM && op2.type == OP_REG) {
            emit_rex(a, op2.reg->size == 8, op2.reg->rex_ext, 0,
                     op1.base ? op1.base->rex_ext : 0);
            emit_text(a, 0x89);
            encode_mem(a, op2.reg->code, &op1);
            return;
        }
        if (op1.type == OP_REG && op2.type == OP_LABEL) {
            emit_rex(a, 1, 0, 0, op1.reg->rex_ext);
            emit_text(a, 0xB8 + op1.reg->code);
            add_reloc(a, SEC_TEXT, a->text_len, op2.label, R_X86_64_64, 0);
            emit_text64(a, 0);
            return;
        }
    }

    /* ALU: add, sub, and, or, xor, cmp, test */
    struct { const char *name; int op_rr; int op_ri_ext; int op_ri; } alu_ops[] = {
        {"add", 0x01, 0, 0x81}, {"sub", 0x29, 5, 0x81},
        {"and", 0x21, 4, 0x81}, {"or",  0x09, 1, 0x81},
        {"xor", 0x31, 6, 0x81}, {"cmp", 0x39, 7, 0x81},
        {"test",0x85, 0, 0xF7},
        {NULL, 0, 0, 0}
    };
    for (int i = 0; alu_ops[i].name; i++) {
        if (strcmp(mnem, alu_ops[i].name) != 0) continue;
        if (op1.type == OP_REG && op2.type == OP_REG) {
            emit_rex(a, op1.reg->size == 8, op2.reg->rex_ext, 0, op1.reg->rex_ext);
            emit_text(a, alu_ops[i].op_rr);
            emit_text(a, modrm(3, op2.reg->code, op1.reg->code));
        } else if (op1.type == OP_REG && op2.type == OP_IMM) {
            emit_rex(a, op1.reg->size == 8, 0, 0, op1.reg->rex_ext);
            emit_text(a, alu_ops[i].op_ri);
            emit_text(a, modrm(3, alu_ops[i].op_ri_ext, op1.reg->code));
            emit_text32(a, (int)op2.imm);
        }
        return;
    }

    /* Shifts */
    if (strcmp(mnem, "shl") == 0 || strcmp(mnem, "shr") == 0 || strcmp(mnem, "sar") == 0) {
        int ext = (mnem[1] == 'h' && mnem[2] == 'l') ? 4 :
                  (mnem[1] == 'h' && mnem[2] == 'r') ? 5 : 7;
        if (op1.type == OP_REG) {
            emit_rex(a, op1.reg->size == 8, 0, 0, op1.reg->rex_ext);
            if (op2.type == OP_IMM) {
                emit_text(a, 0xC1);
                emit_text(a, modrm(3, ext, op1.reg->code));
                emit_text(a, (unsigned char)op2.imm);
            } else {
                emit_text(a, 0xD3);
                emit_text(a, modrm(3, ext, op1.reg->code));
            }
        }
        return;
    }

    /* lea */
    if (strcmp(mnem, "lea") == 0 && op1.type == OP_REG && op2.type == OP_MEM) {
        emit_rex(a, 1, op1.reg->rex_ext, 0, op2.base ? op2.base->rex_ext : 0);
        emit_text(a, 0x8D);
        encode_mem(a, op1.reg->code, &op2);
        return;
    }

    /* mul, div */
    if (strcmp(mnem, "mul") == 0 && op1.type == OP_REG) {
        emit_rex(a, op1.reg->size == 8, 0, 0, op1.reg->rex_ext);
        emit_text(a, 0xF7);
        emit_text(a, modrm(3, 4, op1.reg->code));
        return;
    }
    if ((strcmp(mnem, "div") == 0 || strcmp(mnem, "idiv") == 0) && op1.type == OP_REG) {
        emit_rex(a, op1.reg->size == 8, 0, 0, op1.reg->rex_ext);
        emit_text(a, 0xF7);
        emit_text(a, modrm(3, (mnem[0] == 'i') ? 7 : 6, op1.reg->code));
        return;
    }
    if (strcmp(mnem, "imul") == 0 && op1.type == OP_REG && op2.type == OP_REG) {
        emit_rex(a, op1.reg->size == 8, op1.reg->rex_ext, 0, op2.reg->rex_ext);
        emit_text(a, 0x0F); emit_text(a, 0xAF);
        emit_text(a, modrm(3, op1.reg->code, op2.reg->code));
        return;
    }
    if (strcmp(mnem, "cqo") == 0 || strcmp(mnem, "cdq") == 0) {
        emit_text(a, 0x48); emit_text(a, 0x99);
        return;
    }

    /* call, jmp */
    if (strcmp(mnem, "call") == 0) {
        if (op1.type == OP_LABEL) {
            emit_text(a, 0xE8);
            AsmSymbol *s = find_symbol(a, op1.label);
            if (s && s->defined) {
                int rel = s->offset - (a->text_len + 4);
                emit_text32(a, rel);
            } else {
                add_reloc(a, SEC_TEXT, a->text_len, op1.label, R_X86_64_PLT32, -4);
                emit_text32(a, 0);
            }
        } else if (op1.type == OP_REG) {
            if (op1.reg->rex_ext) emit_text(a, 0x41);
            emit_text(a, 0xFF);
            emit_text(a, modrm(3, 2, op1.reg->code));
        }
        return;
    }
    if (strcmp(mnem, "jmp") == 0) {
        if (op1.type == OP_LABEL) {
            emit_text(a, 0xE9);
            AsmSymbol *s = find_symbol(a, op1.label);
            if (s && s->defined) {
                int rel = s->offset - (a->text_len + 4);
                emit_text32(a, rel);
            } else {
                add_reloc(a, SEC_TEXT, a->text_len, op1.label, R_X86_64_PC32, -4);
                emit_text32(a, 0);
            }
        } else if (op1.type == OP_REG) {
            if (op1.reg->rex_ext) emit_text(a, 0x41);
            emit_text(a, 0xFF);
            emit_text(a, modrm(3, 4, op1.reg->code));
        }
        return;
    }

    /* Conditional jumps */
    struct { const char *name; unsigned char cc; } jcc_ops[] = {
        {"je",0x84},{"jne",0x85},{"jz",0x84},{"jnz",0x85},
        {"jl",0x8C},{"jle",0x8E},{"jg",0x8F},{"jge",0x8D},
        {"jb",0x82},{"jbe",0x86},{"ja",0x87},{"jae",0x83},
        {"js",0x88},{"jns",0x89},{"jo",0x80},{"jno",0x81},
        {NULL,0}
    };
    for (int i = 0; jcc_ops[i].name; i++) {
        if (strcmp(mnem, jcc_ops[i].name) == 0 && op1.type == OP_LABEL) {
            emit_text(a, 0x0F);
            emit_text(a, jcc_ops[i].cc);
            AsmSymbol *s = find_symbol(a, op1.label);
            if (s && s->defined) {
                int rel = s->offset - (a->text_len + 4);
                emit_text32(a, rel);
            } else {
                add_reloc(a, SEC_TEXT, a->text_len, op1.label, R_X86_64_PC32, -4);
                emit_text32(a, 0);
            }
            return;
        }
    }

    /* neg, not, inc, dec */
    if (strcmp(mnem, "neg") == 0 && op1.type == OP_REG) {
        emit_rex(a, op1.reg->size == 8, 0, 0, op1.reg->rex_ext);
        emit_text(a, 0xF7); emit_text(a, modrm(3, 3, op1.reg->code));
        return;
    }
    if (strcmp(mnem, "not") == 0 && op1.type == OP_REG) {
        emit_rex(a, op1.reg->size == 8, 0, 0, op1.reg->rex_ext);
        emit_text(a, 0xF7); emit_text(a, modrm(3, 2, op1.reg->code));
        return;
    }
    if (strcmp(mnem, "inc") == 0 && op1.type == OP_REG) {
        emit_rex(a, op1.reg->size == 8, 0, 0, op1.reg->rex_ext);
        emit_text(a, 0xFF); emit_text(a, modrm(3, 0, op1.reg->code));
        return;
    }
    if (strcmp(mnem, "dec") == 0 && op1.type == OP_REG) {
        emit_rex(a, op1.reg->size == 8, 0, 0, op1.reg->rex_ext);
        emit_text(a, 0xFF); emit_text(a, modrm(3, 1, op1.reg->code));
        return;
    }

    if (mi > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "unknown instruction: %s", mnem);
        asm_error(a, msg);
    }
}

/* ---- ELF object output ---------------------------------------------- */

static void write_le16(unsigned char *buf, unsigned short v) __attribute__((unused));
static void write_le16(unsigned char *buf, unsigned short v) {
    buf[0] = v & 0xFF; buf[1] = (v >> 8) & 0xFF;
}
static void write_le32(unsigned char *buf, unsigned int v) {
    buf[0] = v; buf[1] = v >> 8; buf[2] = v >> 16; buf[3] = v >> 24;
}
static void write_le64(unsigned char *buf, unsigned long v) __attribute__((unused));
static void write_le64(unsigned char *buf, unsigned long v) {
    write_le32(buf, (unsigned int)v);
    write_le32(buf + 4, (unsigned int)(v >> 32));
}

static int write_object(AsmState *a) {
    FILE *f = fopen(a->outfile, "w");
    if (!f) { fprintf(stderr, "as: cannot open %s\n", a->outfile); return -1; }

    /* Section layout: [null] [.text] [.data] [.bss] [.symtab] [.strtab] [.shstrtab] [.rela.text] */
    int nsections = 8;

    /* Build shstrtab */
    char shstrtab[256];
    int shstr_len = 0;
    shstrtab[shstr_len++] = '\0';
    int shname_text = shstr_len; memcpy(shstrtab + shstr_len, ".text", 6); shstr_len += 6;
    int shname_data = shstr_len; memcpy(shstrtab + shstr_len, ".data", 6); shstr_len += 6;
    int shname_bss = shstr_len; memcpy(shstrtab + shstr_len, ".bss", 5); shstr_len += 5;
    int shname_symtab = shstr_len; memcpy(shstrtab + shstr_len, ".symtab", 8); shstr_len += 8;
    int shname_strtab = shstr_len; memcpy(shstrtab + shstr_len, ".strtab", 8); shstr_len += 8;
    int shname_shstrtab = shstr_len; memcpy(shstrtab + shstr_len, ".shstrtab", 10); shstr_len += 10;
    int shname_rela = shstr_len; memcpy(shstrtab + shstr_len, ".rela.text", 11); shstr_len += 11;

    /* Build strtab + symtab */
    char strtab[4096];
    int str_len = 1; strtab[0] = '\0';
    Elf64_Sym symtab[MAX_SYMS + 4];
    int nsym = 0;

    /* Null symbol */
    memset(&symtab[nsym++], 0, sizeof(Elf64_Sym));

    /* Section symbols */
    for (int i = 0; i < 3; i++) {
        Elf64_Sym *s = &symtab[nsym++];
        memset(s, 0, sizeof(*s));
        s->st_info = (STB_LOCAL << 4) | STT_SECTION;
        s->st_shndx = i + 1;
    }
    int first_global = nsym;

    /* User symbols */
    for (int i = 0; i < a->nsyms; i++) {
        Elf64_Sym *s = &symtab[nsym];
        memset(s, 0, sizeof(*s));
        s->st_name = str_len;
        int nlen = (int)strlen(a->syms[i].name);
        memcpy(strtab + str_len, a->syms[i].name, nlen + 1);
        str_len += nlen + 1;
        s->st_info = ((a->syms[i].is_global ? STB_GLOBAL : STB_LOCAL) << 4) | STT_NOTYPE;
        if (a->syms[i].defined) {
            s->st_shndx = a->syms[i].section + 1;
            s->st_value = a->syms[i].offset;
        }
        if (a->syms[i].is_global && !first_global) first_global = nsym;
        nsym++;
    }

    /* Build relocation entries */
    Elf64_Rela relas[MAX_RELOCS];
    int nrela = 0;
    for (int i = 0; i < a->nrelocs; i++) {
        AsmReloc *ar = &a->relocs[i];
        int sym_idx = 0;
        for (int j = 0; j < a->nsyms; j++) {
            if (strcmp(a->syms[j].name, ar->sym_name) == 0) { sym_idx = j + 4; break; }
        }
        Elf64_Rela *r = &relas[nrela++];
        r->r_offset = ar->offset;
        r->r_info = ((unsigned long)sym_idx << 32) | ar->type;
        r->r_addend = ar->addend;
    }

    /* Compute offsets */
    unsigned long off = sizeof(Elf64_Ehdr);
    unsigned long text_off = off; off += a->text_len;
    unsigned long data_off = off; off += a->data_len;
    /* bss has no file data */
    unsigned long symtab_off = off; off += nsym * sizeof(Elf64_Sym);
    unsigned long strtab_off = off; off += str_len;
    unsigned long shstrtab_off = off; off += shstr_len;
    unsigned long rela_off = off; off += nrela * sizeof(Elf64_Rela);
    unsigned long shdr_off = off;

    /* Write ELF header */
    Elf64_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = ELFMAG0;
    ehdr.e_ident[1] = ELFMAG1;
    ehdr.e_ident[2] = ELFMAG2;
    ehdr.e_ident[3] = ELFMAG3;
    ehdr.e_ident[4] = ELFCLASS64;
    ehdr.e_ident[5] = ELFDATA2LSB;
    ehdr.e_ident[6] = 1;
    ehdr.e_type = ET_REL;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = 1;
    ehdr.e_shoff = shdr_off;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum = nsections;
    ehdr.e_shstrndx = 6;
    fwrite(&ehdr, sizeof(ehdr), 1, f);

    /* Section data */
    fwrite(a->text, 1, a->text_len, f);
    fwrite(a->data, 1, a->data_len, f);
    fwrite(symtab, sizeof(Elf64_Sym), nsym, f);
    fwrite(strtab, 1, str_len, f);
    fwrite(shstrtab, 1, shstr_len, f);
    fwrite(relas, sizeof(Elf64_Rela), nrela, f);

    /* Section headers */
    Elf64_Shdr shdr;
    /* [0] null */
    memset(&shdr, 0, sizeof(shdr)); fwrite(&shdr, sizeof(shdr), 1, f);
    /* [1] .text */
    memset(&shdr, 0, sizeof(shdr));
    shdr.sh_name = shname_text; shdr.sh_type = SHT_PROGBITS;
    shdr.sh_flags = SHF_ALLOC | SHF_EXEC;
    shdr.sh_offset = text_off; shdr.sh_size = a->text_len;
    shdr.sh_addralign = 16;
    fwrite(&shdr, sizeof(shdr), 1, f);
    /* [2] .data */
    memset(&shdr, 0, sizeof(shdr));
    shdr.sh_name = shname_data; shdr.sh_type = SHT_PROGBITS;
    shdr.sh_flags = SHF_ALLOC | SHF_WRITE;
    shdr.sh_offset = data_off; shdr.sh_size = a->data_len;
    shdr.sh_addralign = 8;
    fwrite(&shdr, sizeof(shdr), 1, f);
    /* [3] .bss */
    memset(&shdr, 0, sizeof(shdr));
    shdr.sh_name = shname_bss; shdr.sh_type = SHT_NOBITS;
    shdr.sh_flags = SHF_ALLOC | SHF_WRITE;
    shdr.sh_size = a->bss_len; shdr.sh_addralign = 8;
    fwrite(&shdr, sizeof(shdr), 1, f);
    /* [4] .symtab */
    memset(&shdr, 0, sizeof(shdr));
    shdr.sh_name = shname_symtab; shdr.sh_type = SHT_SYMTAB;
    shdr.sh_offset = symtab_off; shdr.sh_size = nsym * sizeof(Elf64_Sym);
    shdr.sh_link = 5; shdr.sh_info = first_global;
    shdr.sh_addralign = 8; shdr.sh_entsize = sizeof(Elf64_Sym);
    fwrite(&shdr, sizeof(shdr), 1, f);
    /* [5] .strtab */
    memset(&shdr, 0, sizeof(shdr));
    shdr.sh_name = shname_strtab; shdr.sh_type = SHT_STRTAB;
    shdr.sh_offset = strtab_off; shdr.sh_size = str_len;
    shdr.sh_addralign = 1;
    fwrite(&shdr, sizeof(shdr), 1, f);
    /* [6] .shstrtab */
    memset(&shdr, 0, sizeof(shdr));
    shdr.sh_name = shname_shstrtab; shdr.sh_type = SHT_STRTAB;
    shdr.sh_offset = shstrtab_off; shdr.sh_size = shstr_len;
    shdr.sh_addralign = 1;
    fwrite(&shdr, sizeof(shdr), 1, f);
    /* [7] .rela.text */
    memset(&shdr, 0, sizeof(shdr));
    shdr.sh_name = shname_rela; shdr.sh_type = SHT_RELA;
    shdr.sh_offset = rela_off; shdr.sh_size = nrela * sizeof(Elf64_Rela);
    shdr.sh_link = 4; shdr.sh_info = 1;
    shdr.sh_addralign = 8; shdr.sh_entsize = sizeof(Elf64_Rela);
    fwrite(&shdr, sizeof(shdr), 1, f);

    fclose(f);
    return 0;
}

/* ---- Main ----------------------------------------------------------- */

int main(int argc, char **argv) {
    AsmState state;
    memset(&state, 0, sizeof(state));
    AsmState *a = &state;
    a->outfile = "a.o";

    const char *infile = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) a->outfile = argv[++i];
        else if (argv[i][0] != '-') infile = argv[i];
    }

    if (!infile) {
        fprintf(stderr, "Usage: as [-o output.o] input.s\n");
        return 1;
    }

    FILE *f = fopen(infile, "r");
    if (!f) { fprintf(stderr, "as: cannot open %s\n", infile); return 1; }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        a->line++;
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        assemble_line(a, line);
        if (a->had_error) break;
    }
    fclose(f);

    if (a->had_error) return 1;

    int rc = write_object(a);
    if (rc == 0)
        printf("as: wrote %s (%d bytes .text, %d bytes .data)\n",
               a->outfile, a->text_len, a->data_len);
    return rc;
}
