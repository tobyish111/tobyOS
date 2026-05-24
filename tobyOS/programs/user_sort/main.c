#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static int reverse_flag = 0;
static int numeric_flag = 0;

static int cmp_lines(const void *a, const void *b) {
    const char *la = *(const char **)a;
    const char *lb = *(const char **)b;
    int r;

    if (numeric_flag) {
        long na = atoi(la);
        long nb = atoi(lb);
        r = (na > nb) - (na < nb);
    } else {
        r = strcmp(la, lb);
    }

    return reverse_flag ? -r : r;
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "rn")) != -1) {
        switch (opt) {
        case 'r': reverse_flag = 1; break;
        case 'n': numeric_flag = 1; break;
        default:
            fprintf(stderr, "usage: sort [-r] [-n] [file]\n");
            return 1;
        }
    }

    FILE *fp = stdin;
    if (optind < argc) {
        fp = fopen(argv[optind], "r");
        if (!fp) {
            fprintf(stderr, "sort: cannot open '%s'\n", argv[optind]);
            return 1;
        }
    }

    char **lines = NULL;
    int count = 0;
    int cap = 0;
    char buf[4096];

    while (fgets(buf, sizeof(buf), fp)) {
        int len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';

        if (count >= cap) {
            cap = cap ? cap * 2 : 256;
            lines = realloc(lines, cap * sizeof(char *));
        }
        lines[count] = malloc(len + 1);
        memcpy(lines[count], buf, len + 1);
        count++;
    }

    if (fp != stdin) fclose(fp);

    qsort(lines, count, sizeof(char *), cmp_lines);

    for (int i = 0; i < count; i++) {
        printf("%s\n", lines[i]);
        free(lines[i]);
    }
    free(lines);
    return 0;
}
