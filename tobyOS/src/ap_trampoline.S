/* ap_trampoline.S -- 16/32/64-bit trampoline for waking secondary CPUs.
 *
 * SIPI delivers control to (vector << 12) in real mode. We pin the
 * trampoline to phys 0x8000 (vector = 0x08) so every label's runtime
 * address is a known constant. The kernel:
 *   1. Reserves phys 0x8000 in the PMM
 *   2. Identity-maps it in the kernel PML4
 *   3. memcpy's the bytes between ap_trampoline_start..ap_trampoline_end
 *      into HHDM(0x8000)
 *   4. Patches the param area
 *   5. Sends INIT-SIPI-SIPI
 *
 * Layout is fixed via .org so we can use plain integer constants in
 * every instruction (LLVM's integrated assembler refuses label
 * arithmetic inside memory operands, so this is the simplest portable
 * way to express "load from this absolute byte offset").
 *
 *   0x000  ap_trampoline_start  -- 16-bit prologue
 *   0x100  pmode_entry          -- 32-bit protected-mode middle
 *   0x200  lmode_entry          -- 64-bit long-mode tail
 *   0x300  tramp_gdt32          -- temp GDT for the 16/32 transition
 *   0x340  tramp_gdtr32         -- pseudo-descriptor pointing at it
 *   0x360  ap_param_pml4   q64  -- patched: kernel PML4 phys (CR3)
 *   0x368  ap_param_stack  q64  -- patched: per-AP RSP top
 *   0x370  ap_param_entry  q64  -- patched: address of ap_entry()
 *   0x378  ap_param_idx    d32  -- patched: cpu_idx (becomes RDI)
 *   0x380  ap_param_gdtr  10b   -- patched: real kernel GDTR copy
 *   0x390  ap_param_idtr  10b   -- patched: real kernel IDTR copy
 *   0x400  ap_trampoline_end    -- (also doubles as scratch-stack top)
 *
 * Why CR0.PG needs the trampoline page identity-mapped: at the instant
 * `mov cr0, eax` enables paging, RIP is at low phys (~0x8000+x). It is
 * immediately re-interpreted as a virt address. If 0x8000 isn't mapped,
 * the very next instruction page-faults -> triple fault. smp.c does the
 * one-page identity map before sending the first SIPI.
 *
 * `lretq` is emitted as raw bytes (REX.W + 0xCB) because LLVM IA does
 * not recognise the mnemonic.
 */

.intel_syntax noprefix

/* Must match AP_TRAMPOLINE_PHYS in include/tobyos/smp.h. We pick 0x70000
 * because (a) it's a SIPI-vector-encodable page (vector = 0x70), and
 * (b) it sits inside Limine's first USABLE memmap region instead of
 * Limine's own bootloader-reclaimable area at 0x1000..0x61000. */
#define TRAMP_PHYS         0x70000

#define OFF_PMODE          0x100
#define OFF_LMODE          0x200
#define OFF_GDT32          0x300
#define OFF_GDTR32         0x340
#define OFF_PARAM_PML4     0x360
#define OFF_PARAM_STACK    0x368
#define OFF_PARAM_ENTRY    0x370
#define OFF_PARAM_IDX      0x378
#define OFF_PARAM_GDTR     0x380
#define OFF_PARAM_IDTR     0x390
#define OFF_END            0x400

/* Section is "a" (allocated only, no exec). The kernel never executes
 * from its own copy of this blob -- it copies the bytes to phys 0x8000
 * and runs from there. Keeping it non-executable from the kernel-virt
 * mapping means a stray jump can't land inside live trampoline. */
.section .ap_trampoline, "a"
.align 4096

.global ap_trampoline_start
.global ap_trampoline_end
.global ap_param_pml4
.global ap_param_stack
.global ap_param_entry
.global ap_param_idx
.global ap_param_gdtr
.global ap_param_idtr

ap_trampoline_start:

/* ===============================================================
 *  REAL MODE  (16-bit, CS=0x800, IP=0)
 * =============================================================== */
.code16
    cli
    cld

    /* DS = CS so all [imm16] reads land in our trampoline page. */
    xor ax, ax
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, OFF_END

    /* Load the temp 32-bit GDT. Operand-size override (0x66) so lgdt
     * loads a 32-bit base from the 6-byte pseudo-descriptor (instead
     * of the default 24-bit base in 16-bit modes). The displacement
     * OFF_GDTR32 is DS-relative -> linear 0x8000 + OFF_GDTR32. */
    .byte 0x66
    lgdt [OFF_GDTR32]

    /* Enter protected mode. */
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    /* Far jump to 32-bit code: 0x66 0xEA imm32 imm16. */
    .byte 0x66, 0xEA
    .long TRAMP_PHYS + OFF_PMODE
    .word 0x08

/* ===============================================================
 *  PROTECTED MODE  (32-bit, paging OFF, flat segments)
 * =============================================================== */
.code32
.org OFF_PMODE
pmode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, TRAMP_PHYS + OFF_END

    /* CR4.PAE = 1 (required for long mode). */
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    /* CR3 = kernel PML4 phys (32-bit absolute load). */
    mov eax, [TRAMP_PHYS + OFF_PARAM_PML4]
    mov cr3, eax

    /* EFER.LME (bit 8) on; preserve EFER.NXE (bit 11). */
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    or  eax, (1 << 11)
    wrmsr

    /* CR0.PG = 1, CR0.WP = 1 -- matches the kernel. After this, RIP is
     * a virtual address; the trampoline page must be identity-mapped. */
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 16)
    mov cr0, eax

    /* Far jump to 64-bit code: 0xEA imm32 imm16. */
    .byte 0xEA
    .long TRAMP_PHYS + OFF_LMODE
    .word 0x18

/* ===============================================================
 *  LONG MODE  (64-bit, kernel PML4, identity-mapped trampoline page)
 *
 *  Use [rbx + disp] so the encoding is unambiguously absolute (LLVM
 *  IA in 64-bit mode prefers RIP-relative for [imm], which would be
 *  wrong since the section's link address differs from the runtime
 *  phys we copied to).
 * =============================================================== */
.code64
.org OFF_LMODE
lmode_entry:
    mov ebx, TRAMP_PHYS         /* zero-extends to rbx = 0x8000 */

    lgdt [rbx + OFF_PARAM_GDTR]
    lidt [rbx + OFF_PARAM_IDTR]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [rbx + OFF_PARAM_STACK]
    mov edi, [rbx + OFF_PARAM_IDX]      /* 1st SysV arg */
    mov rax, [rbx + OFF_PARAM_ENTRY]    /* address of ap_entry() */

    /* Reload CS via lretq: push selector then RIP, then far return.
     * lretq mnemonic isn't accepted by LLVM IA, so we emit the bytes
     * directly: REX.W (0x48) + LRET (0xCB). */
    push 0x08
    push rax
    .byte 0x48, 0xCB

/* ===============================================================
 *  Temp 32-bit GDT (used only across the 16->32->long transition).
 * =============================================================== */
.org OFF_GDT32
tramp_gdt32:
    .quad 0x0000000000000000    /* 0x00 null                      */
    .quad 0x00CF9A000000FFFF    /* 0x08 32-bit code, base=0       */
    .quad 0x00CF92000000FFFF    /* 0x10 32-bit data, base=0       */
    .quad 0x00AF9A000000FFFF    /* 0x18 64-bit long-mode code     */

.org OFF_GDTR32
tramp_gdtr32:
    .word 4 * 8 - 1
    .long TRAMP_PHYS + OFF_GDT32

/* ===============================================================
 *  Patchable parameter area. C side computes the offset of each
 *  symbol within the section and writes through HHDM into the
 *  reserved page at TRAMP_PHYS.
 * =============================================================== */
.org OFF_PARAM_PML4
ap_param_pml4:   .quad 0
.org OFF_PARAM_STACK
ap_param_stack:  .quad 0
.org OFF_PARAM_ENTRY
ap_param_entry:  .quad 0
.org OFF_PARAM_IDX
ap_param_idx:    .long 0
.org OFF_PARAM_GDTR
ap_param_gdtr:   .zero 10
.org OFF_PARAM_IDTR
ap_param_idtr:   .zero 10

.org OFF_END
ap_trampoline_end:

.section .note.GNU-stack, "", @progbits
