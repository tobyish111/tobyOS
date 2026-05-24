#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#define MAX_LINE 4096

int main(int argc, char **argv) {
    int nlines = 10;
    int opt;
    while ((opt = getopt(argc, argv, "n:")) != -1) {
        switch (opt) {
        case 'n':
            nlines = atoi(optarg);
            if (nlines <= 0) nlines = 10;
            break;
        default:
            fprintf(stderr, "usage: tail [-n N] [file]\n");
            return 1;
        }
    }

    FILE *fp = stdin;
    if (optind < argc) {
        fp = fopen(argv[optind], "r");
        if (!fp) {
            fprintf(stderr, "tail: cannot open '%s'\n", argv[optind]);
            return 1;
        }
    }

    char **ring = malloc(nlines * sizeof(char *));
    for (int i = 0; i < nlines; i++) ring[i] = NULL;

    int pos = 0;
    int total = 0;
    char buf[MAX_LINE];

    while (fgets(buf, sizeof(buf), fp)) {
        int len = strlen(buf);
        if (ring[pos]) free(ring[pos]);
        ring[pos] = malloc(len + 1);
        memcpy(ring[pos], buf, len + 1);
        pos = (pos + 1) % nlines;
        total++;
    }

    if (fp != stdin) fclose(fp);

    int start = (total < nlines) ? 0 : pos;
    int count = (total < nlines) ? total : nlines;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % nlines;
        if (ring[idx]) fputs(ring[idx], stdout);
    }

    for (int i = 0; i < nlines; i++)
        if (ring[i]) free(ring[i]);
    free(ring);
    return 0;
}
