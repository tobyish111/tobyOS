#ifndef M36_ELF_H
#define M36_ELF_H
#include <stdint.h>
#include <stddef.h>
int m36_write_elf64(const uint8_t *text, size_t tlen, const uint8_t *ro, size_t rlen, const uint8_t *d,
    size_t dlen, size_t bsz, uint64_t e_entry, uint8_t **out, size_t *out_len);
#endif
