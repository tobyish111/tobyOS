/* hello/main.c -- trivial kernel-mode program for the ELF loader.
 *
 * The program is a tiny .text + .rodata + .data + .bss exercise:
 *   - g_initialised lives in .data and is checked to prove the loader
 *     copied initialised data correctly.
 *   - g_zero_init lives in .bss and is checked to prove the loader
 *     zeroed the memsz tail.
 *   - msg lives in .rodata and is read but not written.
 *   - sum() is a tiny helper to make the entry point non-trivially RIP-
 *     relative, so we exercise the .text mapping with real branches.
 *
 * Returns 0xCAFEBABE on success, or a different sentinel for each
 * failure mode so the kernel test can pinpoint what went wrong.
 *
 * We compile with -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel
 * (matching the kernel itself); the program shares the kernel's stack
 * and runs at CPL=0 -- there is no userspace yet (that's milestone 4).
 */

typedef unsigned int uint32_t;

static const char msg[] = "hello from program (rodata)";

static volatile uint32_t g_initialised = 0xA5A5A5A5u;
static volatile uint32_t g_zero_init;        /* must be 0 from BSS */

static uint32_t sum(uint32_t a, uint32_t b) {
    return a + b;
}

int program_main(void);
int program_main(void) {
    if (g_initialised != 0xA5A5A5A5u)  return 0xBAD00001;   /* .data not loaded */
    if (g_zero_init   != 0u)           return 0xBAD00002;   /* .bss not zeroed */
    if (msg[0] != 'h')                 return 0xBAD00003;   /* .rodata not loaded */

    uint32_t s = sum(0x12340000u, 0x0000ABCDu);
    if (s != 0x1234ABCDu)              return 0xBAD00004;   /* .text not exec */

    /* Mutate .data and read it back -- proves PF_W on the data segment. */
    g_initialised = 0xDEADBEEFu;
    if (g_initialised != 0xDEADBEEFu)  return 0xBAD00005;

    return (int)0xCAFEBABEu;
}
