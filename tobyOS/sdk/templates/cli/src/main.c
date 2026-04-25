/* CLI app template -- replace the body of main with your code.
 *
 * Build:
 *     export TOBYOS_SDK=/path/to/sdk        # if not already set
 *     make
 *
 * Package + install:
 *     $TOBYOS_SDK/tools/pkgbuild tobyapp.toml -o build/myapp.tpkg
 *     # transfer build/myapp.tpkg into /data/repo on the target,
 *     # then on the OS shell:  pkg install /data/repo/myapp.tpkg
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    (void)argv;
    printf("hello from a tobyOS SDK app (argc=%d)\n", argc);
    return 0;
}
