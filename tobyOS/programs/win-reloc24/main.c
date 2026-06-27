/* main.c -- a STOCK Windows .exe that PROVES the loader's base relocation /
 * ASLR path (Track C / C24). Built with `-Wl,--dynamicbase` so the linker sets
 * IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE and keeps the .reloc table; tobyOS then
 * loads it at a RANDOMIZED, 64KB-aligned slide instead of its preferred
 * ImageBase and rewrites every absolute address in the image via the base-reloc
 * fixups.
 *
 * The proof is that absolute pointers baked into the image at link time still
 * work after the slide:
 *
 *   - `fns[]`  : a global array of CODE pointers (&a/&b/&c). Each entry needs a
 *                DIR64 reloc; if the loader mis-fixes them, calling fns[i]()
 *                jumps to the wrong/preferred address and the sum is wrong (or
 *                it faults). A correct fnsum==7 proves code-pointer relocation.
 *   - `msgs[]` : a global array of DATA pointers (string literals in .rdata).
 *                Each needs a DIR64 reloc; printing them + summing their lengths
 *                (==17) proves data-pointer relocation.
 *   - `selfp`  : a global pointer to itself; *selfp==&selfp proves a relocated
 *                pointer round-trips to its own slid address.
 *
 * Exit code 24 iff ALL relocated pointers resolved correctly. The harness also
 * asserts the kernel logged a non-zero ASLR slide (load_base != ImageBase), so
 * a PASS can only happen if relocation actually occurred AND was correct.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

static int a(void) { return 1; }
static int b(void) { return 2; }
static int c(void) { return 4; }

/* Absolute CODE pointers -> .reloc DIR64 fixups. */
static int (*const fns[3])(void) = { a, b, c };

/* Absolute DATA pointers (into .rdata) -> .reloc DIR64 fixups. */
static const char *const msgs[3] = { "alpha", "bravo", "charlie" };

/* A pointer that points at itself: after relocation *selfp must equal &selfp. */
static void *selfp_storage;
static void **const selfp = &selfp_storage;

int main(void) {
    selfp_storage = (void *)&selfp_storage;

    int fnsum = 0;
    for (int i = 0; i < 3; i++) fnsum += fns[i]();          /* expect 7  */

    int len = 0;
    for (int i = 0; i < 3; i++) {
        printf("C24 msg[%d]=%s\n", i, msgs[i]);
        len += (int)strlen(msgs[i]);                        /* expect 17 */
    }

    int self_ok = (*selfp == (void *)&selfp_storage);

    printf("C24 reloc: fnsum=%d len=%d self_ok=%d &fns=%p &msgs=%p main=%p a=%p\n",
           fnsum, len, self_ok,
           (void *)fns, (void *)msgs, (void *)(void *)main, (void *)(void *)a);

    if (fnsum == 7 && len == 17 && self_ok) return 24;
    return 1;
}
