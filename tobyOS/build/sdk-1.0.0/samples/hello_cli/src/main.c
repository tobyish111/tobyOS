/* hello_cli/src/main.c -- minimal CLI sample for the tobyOS SDK 1.0.
 *
 * Demonstrates the smallest reasonable user program built against
 * the SDK: a single TU, libtoby's <stdio.h>, no GUI, exit code 0.
 *
 * Build (out-of-tree):
 *     export TOBYOS_SDK=/path/to/sdk
 *     make
 * Package:
 *     $TOBYOS_SDK/tools/pkgbuild tobyapp.toml -o build/hello_cli.tpkg
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    printf("[hello_cli] Hello from a tobyOS SDK 1.0 app!\n");
    printf("[hello_cli] argc=%d", argc);
    for (int i = 0; i < argc; i++) printf(" argv[%d]='%s'", i, argv[i]);
    printf("\n");

    /* Exit code 0 -> "ALL OK". The shell prints exit codes when
     * non-zero, so a clean exit stays quiet. */
    return 0;
}
