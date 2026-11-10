/* win-miss26 -- Track C, W1: calling an import tobyOS does not implement.
 *
 * GetNumaHighestNodeNumber is a real kernel32 export that no tobyOS shim
 * provides. The loader binds it to a stub that names it on stderr and ends
 * the process with STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139). Before W1 its
 * IAT slot was 0, so the call jumped to address 0 and the process died of a
 * page fault with nothing said about why. Reaching the line after the call
 * is a FAIL (exit 1).
 */
#include <windows.h>
#include <stdio.h>

int main(void) {
    ULONG n = 99;
    printf("calling GetNumaHighestNodeNumber\n");
    BOOL ok = GetNumaHighestNodeNumber(&n);
    printf("returned %d n=%lu -- should not happen\n", (int)ok, (unsigned long)n);
    return 1;
}
