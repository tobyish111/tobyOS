/* The vDSO shared object, embedded verbatim (2026-08-23). Built by the
 * Makefile's build/vdso_image.so rule from tools/vdso/; src/vdso.c
 * parses these bytes as ELF at boot and stages them into shared pages. */
    .section .rodata
    .balign 4096
    .globl vdso_blob_start
    .globl vdso_blob_end
vdso_blob_start:
    .incbin "build/vdso_image.so"
vdso_blob_end:
