/* m36_elf.c -- ET_EXEC: one PT_LOAD (RX) when ro/data/bss all empty. */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <tobyos/elf.h>

#define VBASE 0x400000ul
#define PG 0x1000u
#define FDATA 0x1000ul
#define W32(p, u) do { (p)[0]=(uint8_t)((u)&255);(p)[1]=(uint8_t)(((u)>>8)&255); (p)[2]=(uint8_t)(((u)>>16)&255);(p)[3]=(uint8_t)(((u)>>24)&255);} while (0)
#define W16(p, u) do { (p)[0]=(uint8_t)((u)&255);(p)[1]=(uint8_t)(((u)>>8)&255);} while (0)
#define W64(p, u) do { for (int _i = 0; _i < 8; _i++) (p)[_i] = (uint8_t)(((uint64_t)(u)) >> (8 * _i)); } while (0)
static size_t al(size_t a) { return (a + PG - 1) & ~(size_t)(PG - 1); }
static void wr_eh(uint8_t *B, uint64_t entry, uint32_t nph) {
    memset(B, 0, 64);
    B[0] = ELFMAG0; B[1] = ELFMAG1; B[2] = ELFMAG2; B[3] = ELFMAG3;
    B[4] = ELFCLASS64; B[5] = ELFDATA2LSB; B[6] = EV_CURRENT;
    W16(B + 0x10, ET_EXEC);
    W16(B + 0x12, EM_X86_64);
    W32(B + 0x14, 1u);
    W64(B + 0x18, entry);
    W64(B + 0x20, 64u);
    W64(B + 0x28, 0u);
    W32(B + 0x30, 0u);
    W16(B + 0x34, (uint16_t)sizeof(Elf64_Ehdr));
    W16(B + 0x36, (uint16_t)sizeof(Elf64_Phdr));
    W16(B + 0x38, (uint16_t)nph);
    W16(B + 0x3a, 0u);
    W16(B + 0x3c, 0u);
    W16(B + 0x3e, 0u);
}
static void wr_ph(Elf64_Phdr *p, int type, int fl, uint64_t o, uint64_t v, uint64_t fs, uint64_t ms) {
    p->p_type = (unsigned)type; p->p_flags = (unsigned)fl; p->p_offset = o; p->p_vaddr = v; p->p_paddr = v;
    p->p_filesz = fs; p->p_memsz = ms; p->p_align = 0x1000;
}
int m36_write_elf64(const uint8_t *text, size_t tlen, const uint8_t *ro, size_t rlen, const uint8_t *d,
    size_t dlen, size_t bsz, uint64_t e_entry, uint8_t **out, size_t *out_len) {
    (void)ro;
    (void)d;
    if (!text || tlen == 0 || !out || !out_len) return -1;
    if (!e_entry) e_entry = VBASE;
    if (rlen | dlen | bsz) { /* 3-LOAD path not needed for tobyC stage-1; reject */ return -1; }
    size_t tpad = al(tlen);
    size_t total = (size_t)FDATA + tpad;
    uint8_t *B = (uint8_t *)calloc(1, total);
    if (!B) return -1;
    wr_eh(B, e_entry, 1u);
    Elf64_Phdr *P = (Elf64_Phdr *)(B + 64);
    wr_ph(P, PT_LOAD, PF_R | PF_X, FDATA, VBASE, (uint64_t)tpad, (uint64_t)tpad);
    memcpy(B + FDATA, text, tlen);
    *out = B;
    *out_len = total;
    return 0;
}
