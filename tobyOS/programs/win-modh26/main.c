/* win-modh26 -- Track C, W1: GetModuleHandleA(NULL) is this program's base.
 *
 * A default clang/mingw .exe sets IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, so
 * tobyOS's loader slides it (C24). &__ImageBase is the linker's own record of
 * where the image starts, and it is relocated with the image, so it is the
 * truth to compare against. Exit 26 iff GetModuleHandleA(NULL) agrees and
 * points at an MZ header. Before W1 the shim returned the fixed preferred
 * base 0x140000000 for every program.
 */
#include <windows.h>
#include <stdio.h>

extern IMAGE_DOS_HEADER __ImageBase;

int main(void) {
    HMODULE h = GetModuleHandleA(NULL);
    void *mine = &__ImageBase;
    printf("GetModuleHandleA(NULL)=%p __ImageBase=%p\n", (void *)h, mine);
    if ((void *)h != mine) return 2;
    if (((IMAGE_DOS_HEADER *)h)->e_magic != IMAGE_DOS_SIGNATURE) return 3;
    return 26;
}
