/* host stub: the driver's messages go to stderr, apart from the harness's */
#include <stdio.h>
#define kprintf(...) fprintf(stderr, __VA_ARGS__)
