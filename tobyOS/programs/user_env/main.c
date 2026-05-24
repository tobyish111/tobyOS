#include <stdio.h>

extern char **environ;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (!environ) {
        printf("(no environment)\n");
        return 0;
    }

    for (char **e = environ; *e; e++) {
        printf("%s\n", *e);
    }
    return 0;
}
