/* tobycc.h -- minimal public hook for the in-tree tobycc driver. The
 * compiler is implemented in tobycc.c; this only exposes a test hook. */
#ifndef TOBYOS_TOBYCC_H
#define TOBYOS_TOBYCC_H
#include <stddef.h>
int tobycc_main(int argc, char **argv);
#endif
