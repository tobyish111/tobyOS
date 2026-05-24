/* tcc/main.c -- Minimal C compiler for tobyOS (self-hosting target).
 *
 * A simplified C compiler that can compile a useful subset of C to
 * x86_64 ELF executables. Enough to compile hello-world, simple
 * algorithms, and (aspirationally) a subset of itself.
 *
 * Phases: preprocess -> lex -> parse -> codegen -> emit ELF.
 *
 * Supported types: void, char, int, long, pointers, arrays, structs.
 * Supported statements: if/else, while, for, return, block, expr.
 * Supported expressions: arithmetic, comparison, assignment, calls,
 *    pointer deref/addr-of, array subscript, struct member access.
 * Preprocessor: #include, #define (object macros), #ifdef/#ifndef/#endif.
 * Code generation: x86_64 System V ABI, outputs ELF executable directly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

/* ---- Limits --------------------------------------------------------- */

#define MAX_TOKENS     32768
#define MAX_SYMS       2048
#define MAX_STRINGS    512
#define MAX_CODE       (256*1024)
#define MAX_DATA       (64*1024)
#define MAX_RELOCS     4096
#define MAX_TYPES      512
#define MAX_STRUCT_MEM 32
#define MAX_MACROS     256
#define MAX_INCLUDES   8
#define MAX_LOCALS     128
#define MAX_ARGS       6

/* ---- ELF structures ------------------------------------------------- */

#define ELF_MAGIC      0x464C457F
#define ET_EXEC        2
#define EM_X86_64      62
#define PT_LOAD        1
#define PF_R           4
#define PF_W           2
#define PF_X           1
#define SHT_NULL       0
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3
#define SHT_NOBITS     8

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
    unsigned int p_type;
    unsigned int p_flags;
    unsigned long p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

/* ---- Token types ---------------------------------------------------- */

enum {
    TK_EOF, TK_NUM, TK_STR, TK_IDENT, TK_CHAR,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_BANG,
    TK_LSHIFT, TK_RSHIFT,
    TK_AND, TK_OR,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_ASSIGN, TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ,
    TK_INC, TK_DEC,
    TK_ARROW, TK_DOT,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_SEMICOLON, TK_COLON, TK_ELLIPSIS,
    TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_DO, TK_RETURN, TK_BREAK,
    TK_CONTINUE, TK_SWITCH, TK_CASE, TK_DEFAULT, TK_GOTO,
    TK_SIZEOF,
    TK_INT, TK_CHAR_KW, TK_LONG, TK_SHORT, TK_VOID,
    TK_UNSIGNED, TK_SIGNED, TK_CONST, TK_STATIC, TK_EXTERN,
    TK_STRUCT, TK_UNION, TK_ENUM, TK_TYPEDEF,
    TK_QUESTION
};

typedef struct {
    int type;
    long ival;
    char sval[256];
    int line;
} Token;

/* ---- Type system ---------------------------------------------------- */

enum {
    TY_VOID, TY_CHAR, TY_INT, TY_LONG, TY_PTR, TY_ARRAY, TY_STRUCT, TY_FUNC
};

typedef struct Type {
    int kind;
    int size;
    struct Type *base;       /* for PTR and ARRAY */
    int array_len;
    int struct_id;
    int nparams;
} Type;

typedef struct {
    char name[64];
    Type type;
    int offset;
} StructMember;

typedef struct {
    char name[64];
    StructMember members[MAX_STRUCT_MEM];
    int nmembers;
    int total_size;
} StructDef;

/* ---- Symbol table --------------------------------------------------- */

typedef struct {
    char name[128];
    Type type;
    int is_global;
    int is_defined;
    int offset;       /* stack offset for locals, code offset for globals */
    int is_function;
    int is_extern;
} Symbol;

/* ---- Compiler state ------------------------------------------------- */

typedef struct {
    /* Source */
    char *src;
    int src_len;

    /* Tokens */
    Token tokens[MAX_TOKENS];
    int ntokens;
    int tpos;

    /* Types */
    StructDef structs[MAX_TYPES];
    int nstructs;

    /* Symbols */
    Symbol syms[MAX_SYMS];
    int nsyms;
    int scope_depth;

    /* Local variables */
    int local_offset;
    int max_local_offset;

    /* Code buffer (.text) */
    unsigned char code[MAX_CODE];
    int code_len;

    /* Data buffer (.data + .rodata) */
    unsigned char data[MAX_DATA];
    int data_len;
    unsigned long data_base;

    /* String literals -> data section */
    struct { int offset; char val[256]; } strings[MAX_STRINGS];
    int nstrings;

    /* Fixups for forward references */
    struct { int code_off; char name[128]; int is_rel; } relocs[MAX_RELOCS];
    int nrelocs;

    /* Preprocessor macros */
    struct { char name[64]; char value[512]; } macros[MAX_MACROS];
    int nmacros;

    /* Include paths */
    char include_path[256];

    /* Output file */
    const char *outfile;

    unsigned long text_base;

    int had_error;
    int cur_line;
} TccState;

static void tcc_error(TccState *s, const char *msg) {
    fprintf(stderr, "tcc: line %d: error: %s\n", s->cur_line, msg);
    s->had_error = 1;
}

static void tcc_errorf(TccState *s, const char *fmt, const char *arg) {
    char buf[512];
    snprintf(buf, sizeof(buf), fmt, arg);
    tcc_error(s, buf);
}

/* ---- Preprocessor --------------------------------------------------- */

static void define_macro(TccState *s, const char *name, const char *value) {
    for (int i = 0; i < s->nmacros; i++) {
        if (strcmp(s->macros[i].name, name) == 0) {
            strncpy(s->macros[i].value, value, 511);
            return;
        }
    }
    if (s->nmacros < MAX_MACROS) {
        strncpy(s->macros[s->nmacros].name, name, 63);
        strncpy(s->macros[s->nmacros].value, value, 511);
        s->nmacros++;
    }
}

static const char *find_macro(TccState *s, const char *name) {
    for (int i = 0; i < s->nmacros; i++)
        if (strcmp(s->macros[i].name, name) == 0)
            return s->macros[i].value;
    return NULL;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static char *preprocess(TccState *s, const char *src) {
    size_t cap = strlen(src) * 2 + 4096;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t opos = 0;
    const char *p = src;
    int ifdef_depth = 0;
    int ifdef_skip[32];
    memset(ifdef_skip, 0, sizeof(ifdef_skip));

    while (*p) {
        if (*p == '#' && (p == src || p[-1] == '\n')) {
            p++;
            while (*p == ' ' || *p == '\t') p++;

            if (strncmp(p, "include", 7) == 0 && !isalnum(p[7])) {
                p += 7;
                while (*p == ' ' || *p == '\t') p++;
                char path[256];
                if (*p == '"' || *p == '<') {
                    char end_ch = (*p == '"') ? '"' : '>';
                    p++;
                    int pi = 0;
                    while (*p && *p != end_ch && pi < 255) path[pi++] = *p++;
                    path[pi] = '\0';
                    if (*p == end_ch) p++;

                    if (!ifdef_skip[ifdef_depth]) {
                        char fullpath[512];
                        snprintf(fullpath, sizeof(fullpath), "%s/%s",
                                 s->include_path, path);
                        char *inc_src = read_file(fullpath);
                        if (!inc_src) inc_src = read_file(path);
                        if (inc_src) {
                            char *inc_pp = preprocess(s, inc_src);
                            if (inc_pp) {
                                size_t ilen = strlen(inc_pp);
                                while (opos + ilen + 2 >= cap) {
                                    cap *= 2;
                                    out = realloc(out, cap);
                                }
                                memcpy(out + opos, inc_pp, ilen);
                                opos += ilen;
                                out[opos++] = '\n';
                                free(inc_pp);
                            }
                            free(inc_src);
                        }
                    }
                }
            } else if (strncmp(p, "define", 6) == 0 && !isalnum(p[6])) {
                p += 6;
                while (*p == ' ' || *p == '\t') p++;
                char name[64]; int ni = 0;
                while (*p && !isspace(*p) && ni < 63) name[ni++] = *p++;
                name[ni] = '\0';
                while (*p == ' ' || *p == '\t') p++;
                char value[512]; int vi = 0;
                while (*p && *p != '\n' && vi < 511) value[vi++] = *p++;
                value[vi] = '\0';
                if (!ifdef_skip[ifdef_depth])
                    define_macro(s, name, value);
            } else if (strncmp(p, "ifdef", 5) == 0 && !isalnum(p[5])) {
                p += 5;
                while (*p == ' ' || *p == '\t') p++;
                char name[64]; int ni = 0;
                while (*p && !isspace(*p) && ni < 63) name[ni++] = *p++;
                name[ni] = '\0';
                ifdef_depth++;
                ifdef_skip[ifdef_depth] = ifdef_skip[ifdef_depth-1] || !find_macro(s, name);
            } else if (strncmp(p, "ifndef", 6) == 0 && !isalnum(p[6])) {
                p += 6;
                while (*p == ' ' || *p == '\t') p++;
                char name[64]; int ni = 0;
                while (*p && !isspace(*p) && ni < 63) name[ni++] = *p++;
                name[ni] = '\0';
                ifdef_depth++;
                ifdef_skip[ifdef_depth] = ifdef_skip[ifdef_depth-1] || (find_macro(s, name) != NULL);
            } else if (strncmp(p, "endif", 5) == 0 && !isalnum(p[5])) {
                p += 5;
                if (ifdef_depth > 0) ifdef_depth--;
            } else if (strncmp(p, "else", 4) == 0 && !isalnum(p[4])) {
                p += 4;
                if (ifdef_depth > 0 && !ifdef_skip[ifdef_depth-1])
                    ifdef_skip[ifdef_depth] = !ifdef_skip[ifdef_depth];
            }
            while (*p && *p != '\n') p++;
            if (*p == '\n') { out[opos++] = '\n'; p++; }
        } else if (!ifdef_skip[ifdef_depth]) {
            if (opos + 2 >= cap) { cap *= 2; out = realloc(out, cap); }
            out[opos++] = *p++;
        } else {
            if (*p == '\n') out[opos++] = '\n';
            p++;
        }
    }
    out[opos] = '\0';
    return out;
}

/* ---- Lexer ---------------------------------------------------------- */

static int is_keyword(const char *s) {
    static const struct { const char *name; int tok; } kws[] = {
        {"if",TK_IF},{"else",TK_ELSE},{"while",TK_WHILE},{"for",TK_FOR},
        {"do",TK_DO},{"return",TK_RETURN},{"break",TK_BREAK},
        {"continue",TK_CONTINUE},{"switch",TK_SWITCH},{"case",TK_CASE},
        {"default",TK_DEFAULT},{"goto",TK_GOTO},{"sizeof",TK_SIZEOF},
        {"int",TK_INT},{"char",TK_CHAR_KW},{"long",TK_LONG},{"short",TK_SHORT},
        {"void",TK_VOID},{"unsigned",TK_UNSIGNED},{"signed",TK_SIGNED},
        {"const",TK_CONST},{"static",TK_STATIC},{"extern",TK_EXTERN},
        {"struct",TK_STRUCT},{"union",TK_UNION},{"enum",TK_ENUM},
        {"typedef",TK_TYPEDEF},{NULL,0}
    };
    for (int i = 0; kws[i].name; i++)
        if (strcmp(s, kws[i].name) == 0) return kws[i].tok;
    return 0;
}

static void tokenize(TccState *s) {
    const char *p = s->src;
    int line = 1;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == '\n') { line++; p++; continue; }
        if (!*p) break;

        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) { if (*p == '\n') line++; p++; }
            if (*p) p += 2;
            continue;
        }

        Token *t = &s->tokens[s->ntokens];
        if (s->ntokens >= MAX_TOKENS - 1) break;
        t->line = line;

        if (isdigit(*p) || (*p == '0' && (p[1] == 'x' || p[1] == 'X'))) {
            char *end;
            t->ival = strtol(p, &end, 0);
            p = end;
            t->type = TK_NUM;
            s->ntokens++;
            continue;
        }

        if (*p == '\'') {
            p++;
            if (*p == '\\') {
                p++;
                switch (*p) {
                case 'n': t->ival = '\n'; break;
                case 't': t->ival = '\t'; break;
                case '0': t->ival = '\0'; break;
                case '\\': t->ival = '\\'; break;
                case '\'': t->ival = '\''; break;
                default: t->ival = *p;
                }
            } else t->ival = *p;
            p++;
            if (*p == '\'') p++;
            t->type = TK_CHAR;
            s->ntokens++;
            continue;
        }

        if (*p == '"') {
            p++;
            int i = 0;
            while (*p && *p != '"' && i < 255) {
                if (*p == '\\') {
                    p++;
                    switch (*p) {
                    case 'n': t->sval[i++] = '\n'; break;
                    case 't': t->sval[i++] = '\t'; break;
                    case '0': t->sval[i++] = '\0'; break;
                    case '\\': t->sval[i++] = '\\'; break;
                    case '"': t->sval[i++] = '"'; break;
                    default: t->sval[i++] = *p;
                    }
                } else {
                    if (*p == '\n') line++;
                    t->sval[i++] = *p;
                }
                p++;
            }
            t->sval[i] = '\0';
            if (*p == '"') p++;
            t->type = TK_STR;
            t->ival = i;
            s->ntokens++;
            continue;
        }

        if (isalpha(*p) || *p == '_') {
            int i = 0;
            while ((isalnum(*p) || *p == '_') && i < 255) t->sval[i++] = *p++;
            t->sval[i] = '\0';

            /* Macro expansion */
            const char *macro_val = find_macro(s, t->sval);
            if (macro_val && *macro_val) {
                /* Expand to number if possible */
                char *end;
                long val = strtol(macro_val, &end, 0);
                if (*end == '\0') {
                    t->type = TK_NUM;
                    t->ival = val;
                    s->ntokens++;
                    continue;
                }
            }

            int kw = is_keyword(t->sval);
            t->type = kw ? kw : TK_IDENT;
            s->ntokens++;
            continue;
        }

        /* Multi-char operators */
        char c = *p++;
        switch (c) {
        case '+':
            if (*p == '+') { p++; t->type = TK_INC; }
            else if (*p == '=') { p++; t->type = TK_PLUSEQ; }
            else t->type = TK_PLUS;
            break;
        case '-':
            if (*p == '-') { p++; t->type = TK_DEC; }
            else if (*p == '=') { p++; t->type = TK_MINUSEQ; }
            else if (*p == '>') { p++; t->type = TK_ARROW; }
            else t->type = TK_MINUS;
            break;
        case '*':
            if (*p == '=') { p++; t->type = TK_STAREQ; }
            else t->type = TK_STAR;
            break;
        case '/':
            if (*p == '=') { p++; t->type = TK_SLASHEQ; }
            else t->type = TK_SLASH;
            break;
        case '%': t->type = TK_PERCENT; break;
        case '&':
            if (*p == '&') { p++; t->type = TK_AND; }
            else t->type = TK_AMP;
            break;
        case '|':
            if (*p == '|') { p++; t->type = TK_OR; }
            else t->type = TK_PIPE;
            break;
        case '^': t->type = TK_CARET; break;
        case '~': t->type = TK_TILDE; break;
        case '!':
            if (*p == '=') { p++; t->type = TK_NE; }
            else t->type = TK_BANG;
            break;
        case '=':
            if (*p == '=') { p++; t->type = TK_EQ; }
            else t->type = TK_ASSIGN;
            break;
        case '<':
            if (*p == '=') { p++; t->type = TK_LE; }
            else if (*p == '<') { p++; t->type = TK_LSHIFT; }
            else t->type = TK_LT;
            break;
        case '>':
            if (*p == '=') { p++; t->type = TK_GE; }
            else if (*p == '>') { p++; t->type = TK_RSHIFT; }
            else t->type = TK_GT;
            break;
        case '(': t->type = TK_LPAREN; break;
        case ')': t->type = TK_RPAREN; break;
        case '{': t->type = TK_LBRACE; break;
        case '}': t->type = TK_RBRACE; break;
        case '[': t->type = TK_LBRACKET; break;
        case ']': t->type = TK_RBRACKET; break;
        case ',': t->type = TK_COMMA; break;
        case ';': t->type = TK_SEMICOLON; break;
        case ':': t->type = TK_COLON; break;
        case '?': t->type = TK_QUESTION; break;
        case '.':
            if (*p == '.' && p[1] == '.') { p += 2; t->type = TK_ELLIPSIS; }
            else t->type = TK_DOT;
            break;
        default: continue;
        }
        s->ntokens++;
    }
    s->tokens[s->ntokens].type = TK_EOF;
}

/* ---- Parser helpers ------------------------------------------------- */

static Token *peek(TccState *s) { return &s->tokens[s->tpos]; }
static Token *advance(TccState *s) { return &s->tokens[s->tpos++]; }
static int check(TccState *s, int type) { return peek(s)->type == type; }

static Token *expect_tok(TccState *s, int type, const char *msg) {
    if (!check(s, type)) { tcc_error(s, msg); return peek(s); }
    return advance(s);
}

static int is_type_token(TccState *s) {
    int t = peek(s)->type;
    return t == TK_INT || t == TK_CHAR_KW || t == TK_LONG || t == TK_VOID ||
           t == TK_SHORT || t == TK_UNSIGNED || t == TK_SIGNED ||
           t == TK_STRUCT || t == TK_CONST || t == TK_STATIC || t == TK_EXTERN ||
           t == TK_ENUM;
}

/* ---- Type parsing --------------------------------------------------- */

static Type parse_base_type(TccState *s) {
    Type ty;
    memset(&ty, 0, sizeof(ty));
    int is_unsigned = 0, is_static = 0, is_extern = 0, is_const = 0;
    (void)is_unsigned; (void)is_static; (void)is_extern; (void)is_const;

    while (check(s, TK_CONST) || check(s, TK_STATIC) || check(s, TK_EXTERN) ||
           check(s, TK_UNSIGNED) || check(s, TK_SIGNED)) {
        if (check(s, TK_UNSIGNED)) is_unsigned = 1;
        if (check(s, TK_STATIC)) is_static = 1;
        if (check(s, TK_EXTERN)) is_extern = 1;
        if (check(s, TK_CONST)) is_const = 1;
        advance(s);
    }

    if (check(s, TK_VOID))     { advance(s); ty.kind = TY_VOID; ty.size = 0; }
    else if (check(s, TK_CHAR_KW)) { advance(s); ty.kind = TY_CHAR; ty.size = 1; }
    else if (check(s, TK_SHORT))   { advance(s); ty.kind = TY_INT; ty.size = 2;
        if (check(s, TK_INT)) advance(s); }
    else if (check(s, TK_LONG))    { advance(s); ty.kind = TY_LONG; ty.size = 8;
        if (check(s, TK_LONG)) advance(s);
        if (check(s, TK_INT)) advance(s); }
    else if (check(s, TK_INT))     { advance(s); ty.kind = TY_INT; ty.size = 4; }
    else if (check(s, TK_STRUCT))  {
        advance(s);
        char name[64] = {0};
        if (check(s, TK_IDENT)) {
            strncpy(name, peek(s)->sval, 63);
            advance(s);
        }
        ty.kind = TY_STRUCT;
        ty.struct_id = -1;
        for (int i = 0; i < s->nstructs; i++) {
            if (strcmp(s->structs[i].name, name) == 0) {
                ty.struct_id = i;
                ty.size = s->structs[i].total_size;
                break;
            }
        }
        if (check(s, TK_LBRACE)) {
            advance(s);
            int sid = (ty.struct_id >= 0) ? ty.struct_id : s->nstructs++;
            if (ty.struct_id < 0) {
                strncpy(s->structs[sid].name, name, 63);
                s->structs[sid].nmembers = 0;
                s->structs[sid].total_size = 0;
            }
            ty.struct_id = sid;
            StructDef *sd = &s->structs[sid];
            while (!check(s, TK_RBRACE) && !check(s, TK_EOF)) {
                Type mtype = parse_base_type(s);
                while (check(s, TK_STAR)) { advance(s); mtype.kind = TY_PTR; mtype.size = 8; }
                if (check(s, TK_IDENT) && sd->nmembers < MAX_STRUCT_MEM) {
                    StructMember *m = &sd->members[sd->nmembers++];
                    strncpy(m->name, peek(s)->sval, 63);
                    advance(s);
                    if (check(s, TK_LBRACKET)) {
                        advance(s);
                        int alen = (int)peek(s)->ival;
                        advance(s);
                        expect_tok(s, TK_RBRACKET, "expected ']'");
                        mtype.kind = TY_ARRAY;
                        mtype.array_len = alen;
                        m->type = mtype;
                        m->offset = sd->total_size;
                        sd->total_size += mtype.size * alen;
                    } else {
                        m->type = mtype;
                        m->offset = sd->total_size;
                        sd->total_size += mtype.size;
                    }
                }
                expect_tok(s, TK_SEMICOLON, "expected ';'");
            }
            expect_tok(s, TK_RBRACE, "expected '}'");
            ty.size = sd->total_size;
        }
    } else if (check(s, TK_ENUM)) {
        advance(s);
        if (check(s, TK_IDENT)) advance(s);
        if (check(s, TK_LBRACE)) {
            advance(s);
            while (!check(s, TK_RBRACE) && !check(s, TK_EOF)) {
                if (check(s, TK_IDENT)) {
                    define_macro(s, peek(s)->sval, "");
                    advance(s);
                }
                if (check(s, TK_ASSIGN)) {
                    advance(s);
                    advance(s);
                }
                if (check(s, TK_COMMA)) advance(s);
            }
            expect_tok(s, TK_RBRACE, "expected '}'");
        }
        ty.kind = TY_INT; ty.size = 4;
    } else {
        ty.kind = TY_INT; ty.size = 4;
    }
    return ty;
}

static int type_size(Type *ty) {
    if (ty->kind == TY_PTR) return 8;
    if (ty->kind == TY_ARRAY) return ty->size * ty->array_len;
    return ty->size;
}

/* ---- Code emission (x86_64) ---------------------------------------- */

static void emit_byte(TccState *s, unsigned char b) {
    if (s->code_len < MAX_CODE) s->code[s->code_len++] = b;
}

static void emit_bytes(TccState *s, const unsigned char *b, int n) {
    for (int i = 0; i < n; i++) emit_byte(s, b[i]);
}

static void emit32(TccState *s, int v) {
    emit_byte(s, v & 0xFF);
    emit_byte(s, (v >> 8) & 0xFF);
    emit_byte(s, (v >> 16) & 0xFF);
    emit_byte(s, (v >> 24) & 0xFF);
}

static void emit64(TccState *s, long v) {
    emit32(s, (int)(v & 0xFFFFFFFF));
    emit32(s, (int)((v >> 32) & 0xFFFFFFFF));
}

static void patch32(TccState *s, int offset, int value) {
    s->code[offset]     = value & 0xFF;
    s->code[offset + 1] = (value >> 8) & 0xFF;
    s->code[offset + 2] = (value >> 16) & 0xFF;
    s->code[offset + 3] = (value >> 24) & 0xFF;
}

/* x86_64 register encoding: rax=0, rcx=1, rdx=2, rbx=3, rsp=4, rbp=5, rsi=6, rdi=7 */

static void emit_push_rbp(TccState *s) { emit_byte(s, 0x55); }
static void emit_mov_rsp_rbp(TccState *s) { emit_bytes(s, (unsigned char[]){0x48,0x89,0xE5}, 3); }
static void emit_pop_rbp(TccState *s) { emit_byte(s, 0x5D); }
static void emit_ret(TccState *s) { emit_byte(s, 0xC3); }

static void emit_sub_rsp(TccState *s, int n) {
    if (n == 0) return;
    emit_bytes(s, (unsigned char[]){0x48,0x81,0xEC}, 3);
    emit32(s, n);
}

static void emit_add_rsp(TccState *s, int n) __attribute__((unused));
static void emit_add_rsp(TccState *s, int n) {
    if (n == 0) return;
    emit_bytes(s, (unsigned char[]){0x48,0x81,0xC4}, 3);
    emit32(s, n);
}

static void emit_push_rax(TccState *s) { emit_byte(s, 0x50); }
static void emit_pop_rax(TccState *s) { emit_byte(s, 0x58); }
static void emit_pop_rdi(TccState *s) { emit_byte(s, 0x5F); }
static void emit_pop_rcx(TccState *s) {
    emit_byte(s, 0x59);
}

static void emit_mov_imm_rax(TccState *s, long val) {
    emit_bytes(s, (unsigned char[]){0x48,0xB8}, 2);
    emit64(s, val);
}

static void emit_mov_rbp_off_rax(TccState *s, int off) {
    emit_bytes(s, (unsigned char[]){0x48,0x8B,0x85}, 3);
    emit32(s, off);
}

static void emit_mov_rax_rbp_off(TccState *s, int off) {
    emit_bytes(s, (unsigned char[]){0x48,0x89,0x85}, 3);
    emit32(s, off);
}

static void emit_lea_rbp_off_rax(TccState *s, int off) {
    emit_bytes(s, (unsigned char[]){0x48,0x8D,0x85}, 3);
    emit32(s, off);
}

static void emit_load_rax_deref(TccState *s, int size) {
    if (size == 1) emit_bytes(s, (unsigned char[]){0x48,0x0F,0xB6,0x00}, 4);
    else if (size == 4) emit_bytes(s, (unsigned char[]){0x8B,0x00}, 2);
    else emit_bytes(s, (unsigned char[]){0x48,0x8B,0x00}, 3);
}

static void emit_store_rax_rcx(TccState *s, int size) {
    if (size == 1) emit_bytes(s, (unsigned char[]){0x88,0x01}, 2);
    else if (size == 4) emit_bytes(s, (unsigned char[]){0x89,0x01}, 2);
    else emit_bytes(s, (unsigned char[]){0x48,0x89,0x01}, 3);
}

static void emit_call_rel(TccState *s, int rel) {
    emit_byte(s, 0xE8);
    emit32(s, rel);
}

static void emit_jmp_rel(TccState *s, int rel) {
    emit_byte(s, 0xE9);
    emit32(s, rel);
}

static void emit_jz_rel(TccState *s, int rel) {
    emit_bytes(s, (unsigned char[]){0x0F,0x84}, 2);
    emit32(s, rel);
}

static void emit_jnz_rel(TccState *s, int rel) {
    emit_bytes(s, (unsigned char[]){0x0F,0x85}, 2);
    emit32(s, rel);
}

static void emit_test_rax(TccState *s) {
    emit_bytes(s, (unsigned char[]){0x48,0x85,0xC0}, 3);
}

static void emit_cmp_rcx_rax(TccState *s) {
    emit_bytes(s, (unsigned char[]){0x48,0x39,0xC8}, 3);
}

static void emit_setcc(TccState *s, unsigned char cc) {
    emit_bytes(s, (unsigned char[]){0x0F, cc, 0xC0}, 3);
    emit_bytes(s, (unsigned char[]){0x48,0x0F,0xB6,0xC0}, 4);
}

/* ---- Symbol management ---------------------------------------------- */

static Symbol *find_sym(TccState *s, const char *name) {
    for (int i = s->nsyms - 1; i >= 0; i--)
        if (strcmp(s->syms[i].name, name) == 0) return &s->syms[i];
    return NULL;
}

static Symbol *add_sym(TccState *s, const char *name, Type ty, int is_global) {
    if (s->nsyms >= MAX_SYMS) { tcc_error(s, "too many symbols"); return NULL; }
    Symbol *sym = &s->syms[s->nsyms++];
    memset(sym, 0, sizeof(*sym));
    strncpy(sym->name, name, 127);
    sym->type = ty;
    sym->is_global = is_global;
    return sym;
}

/* ---- Expression codegen (recursive descent) ------------------------- */

static void gen_expr(TccState *s);
static void gen_assign(TccState *s);
static void gen_stmt(TccState *s);

static void gen_lvalue(TccState *s) {
    if (check(s, TK_IDENT)) {
        Symbol *sym = find_sym(s, peek(s)->sval);
        if (!sym) { tcc_errorf(s, "undefined: '%s'", peek(s)->sval); advance(s); return; }
        advance(s);
        if (sym->is_global) emit_mov_imm_rax(s, sym->offset);
        else emit_lea_rbp_off_rax(s, sym->offset);
    } else if (check(s, TK_STAR)) {
        advance(s);
        gen_expr(s);
    }
}

static void gen_primary(TccState *s) {
    if (check(s, TK_NUM) || check(s, TK_CHAR)) {
        emit_mov_imm_rax(s, peek(s)->ival);
        advance(s);
    } else if (check(s, TK_STR)) {
        int str_off = s->data_len;
        int slen = (int)peek(s)->ival;
        memcpy(s->data + s->data_len, peek(s)->sval, slen + 1);
        s->data_len += slen + 1;
        while (s->data_len & 7) s->data[s->data_len++] = 0;
        emit_mov_imm_rax(s, s->data_base + str_off);
        advance(s);
    } else if (check(s, TK_IDENT)) {
        char name[128];
        strncpy(name, peek(s)->sval, 127); name[127] = '\0';
        Symbol *sym = find_sym(s, name);
        advance(s);

        if (check(s, TK_LPAREN)) {
            advance(s);
            int nargs = 0;
            int arg_offsets[MAX_ARGS];
            while (!check(s, TK_RPAREN) && !check(s, TK_EOF)) {
                gen_assign(s);
                emit_push_rax(s);
                if (nargs < MAX_ARGS) arg_offsets[nargs] = 0;
                nargs++;
                if (check(s, TK_COMMA)) advance(s);
            }
            expect_tok(s, TK_RPAREN, "expected ')'");

            /* Pop args into ABI registers (reverse) */
            static const unsigned char reg_pops[][2] = {
                {0x5F,0}, {0x5E,0}, {0x5A,0}, {0x59,0},
            };
            /* r8, r9 need REX */
            for (int i = nargs - 1; i >= 0; i--) {
                if (i == 0) emit_pop_rdi(s);
                else if (i == 1) { emit_byte(s, 0x5E); }
                else if (i == 2) { emit_byte(s, 0x5A); }
                else if (i == 3) emit_pop_rcx(s);
                else if (i == 4) { emit_bytes(s, (unsigned char[]){0x41,0x58}, 2); }
                else if (i == 5) { emit_bytes(s, (unsigned char[]){0x41,0x59}, 2); }
                else emit_pop_rax(s);
            }
            (void)arg_offsets; (void)reg_pops;

            if (sym && sym->is_function && sym->is_defined) {
                int rel = sym->offset - (s->code_len + 5);
                emit_call_rel(s, rel);
            } else {
                if (s->nrelocs < MAX_RELOCS) {
                    s->relocs[s->nrelocs].code_off = s->code_len + 1;
                    strncpy(s->relocs[s->nrelocs].name, name, 127);
                    s->relocs[s->nrelocs].is_rel = 1;
                    s->nrelocs++;
                }
                emit_call_rel(s, 0);
            }
        } else {
            if (!sym) { tcc_errorf(s, "undefined: '%s'", name); return; }
            if (sym->is_global) {
                emit_mov_imm_rax(s, sym->offset);
                if (sym->type.kind != TY_ARRAY)
                    emit_load_rax_deref(s, sym->type.size > 0 ? sym->type.size : 8);
            } else {
                if (sym->type.kind == TY_ARRAY)
                    emit_lea_rbp_off_rax(s, sym->offset);
                else {
                    emit_mov_rbp_off_rax(s, sym->offset);
                }
            }
        }
    } else if (check(s, TK_LPAREN)) {
        advance(s);
        if (is_type_token(s)) {
            parse_base_type(s);
            while (check(s, TK_STAR)) advance(s);
            expect_tok(s, TK_RPAREN, "expected ')'");
            gen_primary(s);
        } else {
            gen_expr(s);
            expect_tok(s, TK_RPAREN, "expected ')'");
        }
    } else if (check(s, TK_AMP)) {
        advance(s);
        gen_lvalue(s);
    } else if (check(s, TK_STAR)) {
        advance(s);
        gen_primary(s);
        emit_load_rax_deref(s, 8);
    } else if (check(s, TK_MINUS)) {
        advance(s);
        gen_primary(s);
        emit_bytes(s, (unsigned char[]){0x48,0xF7,0xD8}, 3);
    } else if (check(s, TK_BANG)) {
        advance(s);
        gen_primary(s);
        emit_test_rax(s);
        emit_setcc(s, 0x94);
    } else if (check(s, TK_TILDE)) {
        advance(s);
        gen_primary(s);
        emit_bytes(s, (unsigned char[]){0x48,0xF7,0xD0}, 3);
    } else if (check(s, TK_SIZEOF)) {
        advance(s);
        int need_paren = check(s, TK_LPAREN);
        if (need_paren) advance(s);
        if (is_type_token(s)) {
            Type ty = parse_base_type(s);
            while (check(s, TK_STAR)) { advance(s); ty.kind = TY_PTR; ty.size = 8; }
            emit_mov_imm_rax(s, type_size(&ty));
        } else {
            emit_mov_imm_rax(s, 8);
            gen_primary(s);
        }
        if (need_paren) expect_tok(s, TK_RPAREN, "expected ')'");
    } else if (check(s, TK_INC) || check(s, TK_DEC)) {
        int is_inc = check(s, TK_INC);
        advance(s);
        gen_lvalue(s);
        emit_push_rax(s);
        emit_load_rax_deref(s, 8);
        if (is_inc) emit_bytes(s, (unsigned char[]){0x48,0xFF,0xC0}, 3);
        else emit_bytes(s, (unsigned char[]){0x48,0xFF,0xC8}, 3);
        emit_bytes(s, (unsigned char[]){0x48,0x89,0xC1}, 3);
        emit_pop_rax(s);
        emit_store_rax_rcx(s, 8);
        emit_bytes(s, (unsigned char[]){0x48,0x89,0xC8}, 3);
    } else {
        tcc_error(s, "unexpected token in expression");
        advance(s);
    }
}

static void gen_postfix(TccState *s) {
    gen_primary(s);
    for (;;) {
        if (check(s, TK_LBRACKET)) {
            advance(s);
            emit_push_rax(s);
            gen_expr(s);
            emit_bytes(s, (unsigned char[]){0x48,0xC1,0xE0,0x03}, 4);
            emit_pop_rcx(s);
            emit_bytes(s, (unsigned char[]){0x48,0x01,0xC8}, 3);
            emit_load_rax_deref(s, 8);
            expect_tok(s, TK_RBRACKET, "expected ']'");
        } else if (check(s, TK_DOT) || check(s, TK_ARROW)) {
            int is_arrow = check(s, TK_ARROW);
            advance(s);
            if (is_arrow) { /* already a pointer, deref not needed */ }
            if (check(s, TK_IDENT)) {
                advance(s);
            }
        } else if (check(s, TK_INC) || check(s, TK_DEC)) {
            advance(s);
        } else break;
    }
}

static void gen_binop(TccState *s, int min_prec) {
    gen_postfix(s);
    while (1) {
        int prec = 0, tok = peek(s)->type;
        switch (tok) {
        case TK_OR:     prec = 1; break;
        case TK_AND:    prec = 2; break;
        case TK_PIPE:   prec = 3; break;
        case TK_CARET:  prec = 4; break;
        case TK_AMP:    prec = 5; break;
        case TK_EQ: case TK_NE: prec = 6; break;
        case TK_LT: case TK_LE: case TK_GT: case TK_GE: prec = 7; break;
        case TK_LSHIFT: case TK_RSHIFT: prec = 8; break;
        case TK_PLUS: case TK_MINUS: prec = 9; break;
        case TK_STAR: case TK_SLASH: case TK_PERCENT: prec = 10; break;
        default: prec = -1;
        }
        if (prec < min_prec) break;
        advance(s);
        emit_push_rax(s);
        gen_binop(s, prec + 1);

        /* rax = right, stack top = left */
        emit_bytes(s, (unsigned char[]){0x48,0x89,0xC1}, 3); /* mov rcx, rax */
        emit_pop_rax(s);

        switch (tok) {
        case TK_PLUS:
            emit_bytes(s, (unsigned char[]){0x48,0x01,0xC8}, 3); break;
        case TK_MINUS:
            emit_bytes(s, (unsigned char[]){0x48,0x29,0xC8}, 3); break;
        case TK_STAR:
            emit_bytes(s, (unsigned char[]){0x48,0x0F,0xAF,0xC1}, 4); break;
        case TK_SLASH:
            emit_bytes(s, (unsigned char[]){0x48,0x99}, 2);
            emit_bytes(s, (unsigned char[]){0x48,0xF7,0xF9}, 3);
            break;
        case TK_PERCENT:
            emit_bytes(s, (unsigned char[]){0x48,0x99}, 2);
            emit_bytes(s, (unsigned char[]){0x48,0xF7,0xF9}, 3);
            emit_bytes(s, (unsigned char[]){0x48,0x89,0xD0}, 3);
            break;
        case TK_AMP:
            emit_bytes(s, (unsigned char[]){0x48,0x21,0xC8}, 3); break;
        case TK_PIPE:
            emit_bytes(s, (unsigned char[]){0x48,0x09,0xC8}, 3); break;
        case TK_CARET:
            emit_bytes(s, (unsigned char[]){0x48,0x31,0xC8}, 3); break;
        case TK_LSHIFT:
            emit_bytes(s, (unsigned char[]){0x48,0xD3,0xE0}, 3); break;
        case TK_RSHIFT:
            emit_bytes(s, (unsigned char[]){0x48,0xD3,0xE8}, 3); break;
        case TK_EQ:
            emit_cmp_rcx_rax(s); emit_setcc(s, 0x94); break;
        case TK_NE:
            emit_cmp_rcx_rax(s); emit_setcc(s, 0x95); break;
        case TK_LT:
            emit_cmp_rcx_rax(s); emit_setcc(s, 0x9C); break;
        case TK_LE:
            emit_cmp_rcx_rax(s); emit_setcc(s, 0x9E); break;
        case TK_GT:
            emit_cmp_rcx_rax(s); emit_setcc(s, 0x9F); break;
        case TK_GE:
            emit_cmp_rcx_rax(s); emit_setcc(s, 0x9D); break;
        case TK_AND:
            emit_test_rax(s);
            emit_jz_rel(s, 0);
            int skip_and = s->code_len;
            emit_bytes(s, (unsigned char[]){0x48,0x89,0xC8}, 3);
            emit_test_rax(s);
            emit_setcc(s, 0x95);
            patch32(s, skip_and - 4, s->code_len - skip_and);
            break;
        case TK_OR:
            emit_test_rax(s);
            emit_jnz_rel(s, 0);
            int skip_or = s->code_len;
            emit_bytes(s, (unsigned char[]){0x48,0x89,0xC8}, 3);
            patch32(s, skip_or - 4, s->code_len - skip_or);
            emit_test_rax(s);
            emit_setcc(s, 0x95);
            break;
        }
    }
}

static void gen_ternary(TccState *s) {
    gen_binop(s, 0);
    if (check(s, TK_QUESTION)) {
        advance(s);
        emit_test_rax(s);
        emit_jz_rel(s, 0);
        int false_jmp = s->code_len;
        gen_expr(s);
        emit_jmp_rel(s, 0);
        int end_jmp = s->code_len;
        expect_tok(s, TK_COLON, "expected ':'");
        patch32(s, false_jmp - 4, s->code_len - false_jmp);
        gen_ternary(s);
        patch32(s, end_jmp - 4, s->code_len - end_jmp);
    }
}

static void gen_assign(TccState *s) {
    /* Check for assignment pattern: ident = expr */
    if (check(s, TK_IDENT) && s->tokens[s->tpos + 1].type == TK_ASSIGN) {
        Symbol *sym = find_sym(s, peek(s)->sval);
        if (sym) {
            advance(s); advance(s);
            gen_assign(s);
            if (sym->is_global) {
                emit_push_rax(s);
                emit_mov_imm_rax(s, sym->offset);
                emit_bytes(s, (unsigned char[]){0x48,0x89,0xC1}, 3);
                emit_pop_rax(s);
                emit_store_rax_rcx(s, sym->type.size > 0 ? sym->type.size : 8);
            } else {
                emit_mov_rax_rbp_off(s, sym->offset);
            }
            return;
        }
    }
    if (check(s, TK_STAR)) {
        int save = s->tpos;
        advance(s);
        if (check(s, TK_IDENT) && s->tokens[s->tpos + 1].type == TK_ASSIGN) {
            Symbol *sym = find_sym(s, peek(s)->sval);
            if (sym) {
                advance(s); advance(s);
                gen_assign(s);
                emit_push_rax(s);
                if (sym->is_global)
                    emit_mov_imm_rax(s, sym->offset);
                else
                    emit_mov_rbp_off_rax(s, sym->offset);
                emit_bytes(s, (unsigned char[]){0x48,0x89,0xC1}, 3);
                emit_pop_rax(s);
                emit_store_rax_rcx(s, 8);
                return;
            }
        }
        s->tpos = save;
    }
    gen_ternary(s);
}

static void gen_expr(TccState *s) {
    gen_assign(s);
}

/* ---- Statement codegen ---------------------------------------------- */

static int break_targets[64];
static int continue_targets[64];
static int loop_depth;

static void gen_stmt(TccState *s) {
    if (s->had_error) return;

    if (check(s, TK_LBRACE)) {
        advance(s);
        int saved_nsyms = s->nsyms;
        while (!check(s, TK_RBRACE) && !check(s, TK_EOF)) gen_stmt(s);
        expect_tok(s, TK_RBRACE, "expected '}'");
        s->nsyms = saved_nsyms;
        return;
    }

    if (check(s, TK_IF)) {
        advance(s);
        expect_tok(s, TK_LPAREN, "expected '('");
        gen_expr(s);
        expect_tok(s, TK_RPAREN, "expected ')'");
        emit_test_rax(s);
        emit_jz_rel(s, 0);
        int else_jmp = s->code_len;
        gen_stmt(s);
        if (check(s, TK_ELSE)) {
            advance(s);
            emit_jmp_rel(s, 0);
            int end_jmp = s->code_len;
            patch32(s, else_jmp - 4, s->code_len - else_jmp);
            gen_stmt(s);
            patch32(s, end_jmp - 4, s->code_len - end_jmp);
        } else {
            patch32(s, else_jmp - 4, s->code_len - else_jmp);
        }
        return;
    }

    if (check(s, TK_WHILE)) {
        advance(s);
        int loop_start = s->code_len;
        expect_tok(s, TK_LPAREN, "expected '('");
        gen_expr(s);
        expect_tok(s, TK_RPAREN, "expected ')'");
        emit_test_rax(s);
        emit_jz_rel(s, 0);
        int exit_jmp = s->code_len;
        loop_depth++;
        break_targets[loop_depth] = 0;
        continue_targets[loop_depth] = loop_start;
        gen_stmt(s);
        emit_jmp_rel(s, loop_start - (s->code_len + 5));
        patch32(s, exit_jmp - 4, s->code_len - exit_jmp);
        if (break_targets[loop_depth])
            patch32(s, break_targets[loop_depth] - 4, s->code_len - break_targets[loop_depth]);
        loop_depth--;
        return;
    }

    if (check(s, TK_FOR)) {
        advance(s);
        expect_tok(s, TK_LPAREN, "expected '('");
        if (!check(s, TK_SEMICOLON)) {
            if (is_type_token(s)) {
                Type ty = parse_base_type(s);
                while (check(s, TK_STAR)) { advance(s); ty.kind = TY_PTR; ty.size = 8; }
                if (check(s, TK_IDENT)) {
                    s->local_offset -= 8;
                    Symbol *sym = add_sym(s, peek(s)->sval, ty, 0);
                    if (sym) sym->offset = s->local_offset;
                    advance(s);
                    if (check(s, TK_ASSIGN)) {
                        advance(s);
                        gen_expr(s);
                        if (sym) emit_mov_rax_rbp_off(s, sym->offset);
                    }
                }
            } else gen_expr(s);
        }
        expect_tok(s, TK_SEMICOLON, "expected ';'");
        int cond_start = s->code_len;
        int exit_jmp = 0;
        if (!check(s, TK_SEMICOLON)) {
            gen_expr(s);
            emit_test_rax(s);
            emit_jz_rel(s, 0);
            exit_jmp = s->code_len;
        }
        expect_tok(s, TK_SEMICOLON, "expected ';'");
        int incr_start = s->tpos;
        if (!check(s, TK_RPAREN)) {
            /* Skip increment in first pass, we'll emit it after body */
            int depth = 0;
            while (!check(s, TK_EOF)) {
                if (check(s, TK_LPAREN)) depth++;
                else if (check(s, TK_RPAREN)) { if (depth == 0) break; depth--; }
                advance(s);
            }
        }
        expect_tok(s, TK_RPAREN, "expected ')'");
        loop_depth++;
        gen_stmt(s);
        /* Now emit increment */
        int save_tpos = s->tpos;
        s->tpos = incr_start;
        if (!check(s, TK_RPAREN)) gen_expr(s);
        s->tpos = save_tpos;
        emit_jmp_rel(s, cond_start - (s->code_len + 5));
        if (exit_jmp) patch32(s, exit_jmp - 4, s->code_len - exit_jmp);
        loop_depth--;
        return;
    }

    if (check(s, TK_RETURN)) {
        advance(s);
        if (!check(s, TK_SEMICOLON)) gen_expr(s);
        else emit_mov_imm_rax(s, 0);
        expect_tok(s, TK_SEMICOLON, "expected ';'");
        /* Epilogue */
        emit_bytes(s, (unsigned char[]){0x48,0x89,0xEC}, 3);
        emit_pop_rbp(s);
        emit_ret(s);
        return;
    }

    if (check(s, TK_BREAK)) {
        advance(s);
        expect_tok(s, TK_SEMICOLON, "expected ';'");
        emit_jmp_rel(s, 0);
        break_targets[loop_depth] = s->code_len;
        return;
    }

    if (check(s, TK_CONTINUE)) {
        advance(s);
        expect_tok(s, TK_SEMICOLON, "expected ';'");
        emit_jmp_rel(s, continue_targets[loop_depth] - (s->code_len + 5));
        return;
    }

    /* Local variable declaration */
    if (is_type_token(s)) {
        Type ty = parse_base_type(s);
        while (check(s, TK_STAR)) { advance(s); ty.kind = TY_PTR; ty.size = 8; }
        while (1) {
            if (!check(s, TK_IDENT)) break;
            char vname[128];
            strncpy(vname, peek(s)->sval, 127); vname[127] = '\0';
            advance(s);
            int sz = type_size(&ty);
            if (sz < 8) sz = 8;
            if (check(s, TK_LBRACKET)) {
                advance(s);
                int alen = (int)peek(s)->ival;
                advance(s);
                expect_tok(s, TK_RBRACKET, "expected ']'");
                Type aty = ty;
                aty.kind = TY_ARRAY;
                aty.array_len = alen;
                sz = type_size(&aty) < 8 ? 8 : type_size(&aty);
                s->local_offset -= sz;
                Symbol *sym = add_sym(s, vname, aty, 0);
                if (sym) sym->offset = s->local_offset;
            } else {
                s->local_offset -= sz;
                Symbol *sym = add_sym(s, vname, ty, 0);
                if (sym) sym->offset = s->local_offset;
                if (check(s, TK_ASSIGN)) {
                    advance(s);
                    gen_expr(s);
                    if (sym) emit_mov_rax_rbp_off(s, sym->offset);
                }
            }
            if (!check(s, TK_COMMA)) break;
            advance(s);
        }
        expect_tok(s, TK_SEMICOLON, "expected ';'");
        return;
    }

    /* Expression statement */
    gen_expr(s);
    expect_tok(s, TK_SEMICOLON, "expected ';'");
}

/* ---- Top-level parsing ---------------------------------------------- */

static void gen_function(TccState *s, const char *name, Type ret_type) {
    Symbol *sym = find_sym(s, name);
    if (!sym) sym = add_sym(s, name, ret_type, 1);
    if (sym) { sym->is_function = 1; sym->offset = s->code_len; sym->is_defined = 1; }

    int saved_nsyms = s->nsyms;
    s->local_offset = 0;

    expect_tok(s, TK_LPAREN, "expected '('");

    /* Parse parameters */
    int nparam = 0;
    while (!check(s, TK_RPAREN) && !check(s, TK_EOF)) {
        if (check(s, TK_ELLIPSIS)) { advance(s); break; }
        Type pty = parse_base_type(s);
        while (check(s, TK_STAR)) { advance(s); pty.kind = TY_PTR; pty.size = 8; }
        if (check(s, TK_IDENT)) {
            s->local_offset -= 8;
            Symbol *psym = add_sym(s, peek(s)->sval, pty, 0);
            if (psym) psym->offset = s->local_offset;
            advance(s);
        }
        nparam++;
        if (check(s, TK_COMMA)) advance(s);
    }
    expect_tok(s, TK_RPAREN, "expected ')'");

    if (check(s, TK_SEMICOLON)) {
        /* Forward declaration */
        advance(s);
        if (sym) sym->is_defined = 0;
        s->nsyms = saved_nsyms;
        return;
    }

    /* Function body */
    emit_push_rbp(s);
    emit_mov_rsp_rbp(s);
    int stack_patch = s->code_len;
    emit_sub_rsp(s, 0);

    /* Store ABI register args into parameter slots */
    static const unsigned char param_stores[][4] = {
        {0x48,0x89,0xBD,0}, /* mov [rbp+off], rdi */
        {0x48,0x89,0xB5,0}, /* mov [rbp+off], rsi */
        {0x48,0x89,0x95,0}, /* mov [rbp+off], rdx */
        {0x48,0x89,0x8D,0}, /* mov [rbp+off], rcx */
    };
    int pidx = 0;
    for (int i = saved_nsyms; i < s->nsyms && pidx < 4; i++) {
        if (!s->syms[i].is_global) {
            emit_bytes(s, param_stores[pidx], 3);
            emit32(s, s->syms[i].offset);
            pidx++;
        }
    }
    if (nparam > 4 && pidx == 4) {
        for (int i = saved_nsyms + 4; i < s->nsyms; i++) {
            if (!s->syms[i].is_global && (i - saved_nsyms) == 4) {
                emit_bytes(s, (unsigned char[]){0x4C,0x89,0x85}, 3);
                emit32(s, s->syms[i].offset);
            }
            if (!s->syms[i].is_global && (i - saved_nsyms) == 5) {
                emit_bytes(s, (unsigned char[]){0x4C,0x89,0x8D}, 3);
                emit32(s, s->syms[i].offset);
            }
        }
    }

    expect_tok(s, TK_LBRACE, "expected '{'");
    while (!check(s, TK_RBRACE) && !check(s, TK_EOF)) gen_stmt(s);
    expect_tok(s, TK_RBRACE, "expected '}'");

    /* Implicit return 0 */
    emit_mov_imm_rax(s, 0);
    emit_bytes(s, (unsigned char[]){0x48,0x89,0xEC}, 3);
    emit_pop_rbp(s);
    emit_ret(s);

    /* Patch stack frame size */
    int frame = (-s->local_offset + 15) & ~15;
    if (frame < 16) frame = 16;
    patch32(s, stack_patch + 3, frame);

    s->nsyms = saved_nsyms;
    /* Re-add the function symbol */
    if (sym) { sym->is_defined = 1; sym->offset = sym->offset; }
}

static void gen_global(TccState *s) {
    int is_extern = 0, is_typedef = 0;
    if (check(s, TK_EXTERN)) { is_extern = 1; advance(s); }
    if (check(s, TK_TYPEDEF)) { is_typedef = 1; advance(s); }

    Type ty = parse_base_type(s);
    while (check(s, TK_STAR)) { advance(s); ty.kind = TY_PTR; ty.size = 8; }

    if (is_typedef) {
        if (check(s, TK_IDENT)) advance(s);
        expect_tok(s, TK_SEMICOLON, "expected ';'");
        return;
    }

    if (!check(s, TK_IDENT)) {
        expect_tok(s, TK_SEMICOLON, "expected ';'");
        return;
    }

    char name[128];
    strncpy(name, peek(s)->sval, 127); name[127] = '\0';
    advance(s);

    if (check(s, TK_LPAREN)) {
        if (is_extern) {
            Symbol *sym = add_sym(s, name, ty, 1);
            if (sym) { sym->is_function = 1; sym->is_extern = 1; }
            /* Skip to semicolon */
            int depth = 1;
            advance(s);
            while (!check(s, TK_EOF) && depth > 0) {
                if (check(s, TK_LPAREN)) depth++;
                if (check(s, TK_RPAREN)) depth--;
                advance(s);
            }
            expect_tok(s, TK_SEMICOLON, "expected ';'");
            return;
        }
        gen_function(s, name, ty);
        return;
    }

    /* Global variable */
    int data_off = s->data_len;
    int sz = type_size(&ty);
    if (sz < 8) sz = 8;
    Symbol *sym = add_sym(s, name, ty, 1);
    if (sym) sym->offset = (int)(s->data_base + data_off);

    if (check(s, TK_ASSIGN)) {
        advance(s);
        if (check(s, TK_NUM) || check(s, TK_CHAR)) {
            long val = peek(s)->ival;
            advance(s);
            memcpy(s->data + s->data_len, &val, sz);
        } else if (check(s, TK_STR)) {
            int str_off = s->data_len + sz;
            memcpy(s->data + str_off, peek(s)->sval, peek(s)->ival + 1);
            long addr = (long)(s->data_base + str_off);
            memcpy(s->data + s->data_len, &addr, 8);
            s->data_len = str_off + (int)peek(s)->ival + 1;
            while (s->data_len & 7) s->data[s->data_len++] = 0;
            advance(s);
        }
    }
    s->data_len += sz;
    while (s->data_len & 7) s->data[s->data_len++] = 0;

    expect_tok(s, TK_SEMICOLON, "expected ';'");
}

/* ---- ELF output ----------------------------------------------------- */

static int write_elf(TccState *s) {
    FILE *f = fopen(s->outfile, "w");
    if (!f) {
        fprintf(stderr, "tcc: cannot open %s for writing\n", s->outfile);
        return -1;
    }

    unsigned long base = 0x400000;
    s->text_base = base + sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr);
    s->data_base = s->text_base + ((s->code_len + 0xFFF) & ~0xFFF);

    /* Re-run to fix up data_base addresses -- in a real compiler we'd
     * do a two-pass or use relocations. For now, the addresses were set
     * during code generation. */

    Elf64_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = 0x7F;
    ehdr.e_ident[1] = 'E';
    ehdr.e_ident[2] = 'L';
    ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = 2;  /* 64-bit */
    ehdr.e_ident[5] = 1;  /* little-endian */
    ehdr.e_ident[6] = 1;  /* ELF version */
    ehdr.e_type = ET_EXEC;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = 1;
    ehdr.e_phoff = sizeof(Elf64_Ehdr);
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum = 2;

    /* Find _start or main as entry */
    Symbol *entry_sym = find_sym(s, "_start");
    if (!entry_sym) entry_sym = find_sym(s, "main");
    ehdr.e_entry = entry_sym ? (s->text_base + entry_sym->offset) : s->text_base;

    /* Text segment */
    Elf64_Phdr text_phdr;
    memset(&text_phdr, 0, sizeof(text_phdr));
    text_phdr.p_type = PT_LOAD;
    text_phdr.p_flags = PF_R | PF_X;
    text_phdr.p_offset = 0;
    text_phdr.p_vaddr = base;
    text_phdr.p_paddr = base;
    unsigned long text_filesz = sizeof(Elf64_Ehdr) + 2*sizeof(Elf64_Phdr) + s->code_len;
    text_phdr.p_filesz = text_filesz;
    text_phdr.p_memsz = text_filesz;
    text_phdr.p_align = 0x1000;

    /* Data segment */
    unsigned long data_file_offset = (text_filesz + 0xFFF) & ~0xFFF;
    Elf64_Phdr data_phdr;
    memset(&data_phdr, 0, sizeof(data_phdr));
    data_phdr.p_type = PT_LOAD;
    data_phdr.p_flags = PF_R | PF_W;
    data_phdr.p_offset = data_file_offset;
    data_phdr.p_vaddr = s->data_base;
    data_phdr.p_paddr = s->data_base;
    data_phdr.p_filesz = s->data_len;
    data_phdr.p_memsz = s->data_len + 4096;
    data_phdr.p_align = 0x1000;

    fwrite(&ehdr, sizeof(ehdr), 1, f);
    fwrite(&text_phdr, sizeof(text_phdr), 1, f);
    fwrite(&data_phdr, sizeof(data_phdr), 1, f);
    fwrite(s->code, 1, s->code_len, f);

    /* Pad to data segment offset */
    unsigned long cur = sizeof(Elf64_Ehdr) + 2*sizeof(Elf64_Phdr) + s->code_len;
    while (cur < data_file_offset) { fputc(0, f); cur++; }

    fwrite(s->data, 1, s->data_len, f);

    fclose(f);
    return 0;
}

/* ---- Main ----------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr, "Usage: tcc [-o output] [-I include_path] [-D macro[=val]] input.c\n");
}

int main(int argc, char **argv) {
    TccState state;
    memset(&state, 0, sizeof(state));
    TccState *s = &state;
    s->outfile = "a.out";
    strncpy(s->include_path, "/include", 255);

    const char *infile = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            s->outfile = argv[++i];
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            strncpy(s->include_path, argv[++i], 255);
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            strncpy(s->include_path, argv[i] + 2, 255);
        } else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            char *eq = strchr(argv[++i], '=');
            if (eq) { *eq = '\0'; define_macro(s, argv[i], eq+1); *eq = '='; }
            else define_macro(s, argv[i], "1");
        } else if (strncmp(argv[i], "-D", 2) == 0) {
            char *arg = argv[i] + 2;
            char *eq = strchr(arg, '=');
            if (eq) { *eq = '\0'; define_macro(s, arg, eq+1); *eq = '='; }
            else define_macro(s, arg, "1");
        } else if (strcmp(argv[i], "-c") == 0) {
            /* Compile only -- stub */
        } else if (argv[i][0] != '-') {
            infile = argv[i];
        }
    }

    if (!infile) { usage(); return 1; }

    char *raw = read_file(infile);
    if (!raw) { fprintf(stderr, "tcc: cannot open %s\n", infile); return 1; }

    s->data_base = 0x600000;

    define_macro(s, "__tobyos__", "1");
    define_macro(s, "__x86_64__", "1");
    define_macro(s, "__LP64__", "1");
    define_macro(s, "NULL", "((void*)0)");

    s->src = preprocess(s, raw);
    free(raw);
    if (!s->src) { fprintf(stderr, "tcc: preprocess failed\n"); return 1; }

    tokenize(s);

    while (!check(s, TK_EOF) && !s->had_error) {
        gen_global(s);
    }

    if (s->had_error) {
        free(s->src);
        return 1;
    }

    /* Resolve relocations */
    for (int i = 0; i < s->nrelocs; i++) {
        Symbol *sym = find_sym(s, s->relocs[i].name);
        if (sym && sym->is_function) {
            int target = sym->offset;
            int from = s->relocs[i].code_off + 4;
            patch32(s, s->relocs[i].code_off, target - from);
        }
    }

    int rc = write_elf(s);
    free(s->src);
    if (rc == 0 && !s->had_error) {
        printf("tcc: wrote %s (%d bytes code, %d bytes data)\n",
               s->outfile, s->code_len, s->data_len);
    }
    return s->had_error ? 1 : rc;
}
