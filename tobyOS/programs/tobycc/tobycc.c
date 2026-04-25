/* tobycc.c -- Milestone 36 "TobyC" stage-1: static ET_EXEC for tobyOS.
 * Accepts: one or more .c files concatenated; the combined source must
 * contain:   int main(void) { return N; }   or   int main() { return N; }
 * with optional whitespace. No other C syntax in this stage.
 * Output: raw ELF to -o (valid ET_EXEC for the kernel's elf_load_user). */
#include "tobycc.h"
#include "m36_elf.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h> /* SEEK_SET, SEEK_END for fseek(3) in freestanding stdio */

#define V0 0x400000ul

static void die(const char *f, ...) {
    va_list a;
    va_start(a, f);
    vfprintf(stderr, f, a);
    fputc('\n', stderr);
    va_end(a);
    exit(1);
}

/* Parse "return" followed by a decimal in src */
static int parse_return_const(const char *s, int *nout) {
    const char *p = s;
    for (;;) {
        const char *q = strstr(p, "return");
        if (!q) return 0;
        p = q + 6;
        while (isspace((unsigned char)*p)) p++;
        if (*p == 0) return 0;
        char *end = NULL;
        long v = strtol(p, &end, 0);
        if (end == p) { p = q + 1; continue; }
        *nout = (int)v;
        return 1;
    }
}

/* Build _start (SysV) + main: mov rdi,[rsp]; lea rsi,[rsp+8]; call main; mov edi,eax; xor rax; syscall(0) */
/* main: mov eax, N; ret */
static int emit_trivial(int N, uint8_t **tout, size_t *tlen) {
    uint8_t b[64];
    int i = 0;
    /* _start at vaddr V0+0 */
    b[i++] = 0x48; b[i++] = 0x8b; b[i++] = 0x3c; b[i++] = 0x24;          /* mov rdi, [rsp] */
    b[i++] = 0x48; b[i++] = 0x8d; b[i++] = 0x74; b[i++] = 0x24; b[i++] = 0x08; /* lea rsi, [rsp+8] */
    int csite = i;
    b[i++] = 0xe8; b[i++] = 0; b[i++] = 0; b[i++] = 0; b[i++] = 0;            /* rel32 to main (patch) */
    b[i++] = 0x89; b[i++] = 0xc7;                                      /* mov edi, eax */
    b[i++] = 0x48; b[i++] = 0x31; b[i++] = 0xc0;                       /* xor rax, rax; SYS_EXIT=0 */
    b[i++] = 0x0f; b[i++] = 0x05;                                     /* syscall */
    b[i++] = 0x0f; b[i++] = 0x0b;                                     /* ud2 */
    int main_off = i;
    b[i++] = 0xb8; b[i++] = (uint8_t)(N & 255); b[i++] = (uint8_t)((N >> 8) & 255);
    b[i++] = (uint8_t)((N >> 16) & 255); b[i++] = (uint8_t)((N >> 24) & 255);   /* mov eax, N */
    b[i++] = 0xc3;                                           /* ret */
    /* patch call rel: target = V0+main_off, RIP after insn = V0+csite+5 */
    int32_t rel = (int32_t)((V0 + (uint64_t)main_off) - (V0 + (uint64_t)csite + 5u));
    b[csite + 1] = (uint8_t)(rel & 0xff);
    b[csite + 2] = (uint8_t)((rel >> 8) & 0xff);
    b[csite + 3] = (uint8_t)((rel >> 16) & 0xff);
    b[csite + 4] = (uint8_t)((rel >> 24) & 0xff);
    *tout = (uint8_t *)malloc((size_t)i);
    if (!*tout) return -1;
    memcpy(*tout, b, (size_t)i);
    *tlen = (size_t)i;
    return 0;
}

int tobycc_main(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "-o")) {
        die("usage: tobycc -o out.elf a.c [b.c ...]");
    }
    const char *outp = argv[2];
    size_t cap = 1, tot = 0;
    char *all = (char *)malloc(cap);
    if (!all) die("oom");
    all[0] = 0;
    for (int f = 3; f < argc; f++) {
        FILE *fp = fopen(argv[f], "r");
        if (!fp) die("open %s: %s", argv[f], strerror(errno));
        fseek(fp, 0, SEEK_END);
        long m = ftell(fp);
        if (m < 0) die("fseek");
        fseek(fp, 0, SEEK_SET);
        while (tot + (size_t)m + 32 > cap) { cap *= 2; all = (char *)realloc(all, cap); if (!all) die("oom"); }
        int k = (int)fread(all + tot, 1, (size_t)m, fp);
        fclose(fp);
        tot += (size_t)k;
        all[tot] = 0;
    }
    if (tot == 0) die("no source");
    int N = 0;
    if (!parse_return_const(all, &N))
        die("TobyC stage-1: need `int main(void){ return N; }` (or int main() ...)");
    uint8_t *text = 0; size_t tlen = 0;
    if (emit_trivial(N, &text, &tlen) < 0) die("emit");
    uint8_t *elf = 0; size_t elen = 0;
    if (m36_write_elf64(text, tlen, NULL, 0, NULL, 0, 0, V0, &elf, &elen) < 0)
        die("elf");
    free(text);
    FILE *o = fopen(outp, "wb");
    if (!o) die("open %s: %s", outp, strerror(errno));
    if (fwrite(elf, 1, elen, o) != elen) die("write");
    fclose(o);
    free(elf);
    free(all);
    return 0;
}

int main(int argc, char **argv) { return tobycc_main(argc, argv); }
