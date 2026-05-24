#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#define MAX_LINE 4096

int main(int argc, char **argv) {
    int count_flag = 0;
    int opt;
    while ((opt = getopt(argc, argv, "c")) != -1) {
        switch (opt) {
        case 'c': count_flag = 1; break;
        default:
            fprintf(stderr, "usage: uniq [-c] [file]\n");
            return 1;
        }
    }

    FILE *fp = stdin;
    if (optind < argc) {
        fp = fopen(argv[optind], "r");
        if (!fp) {
            fprintf(stderr, "uniq: cannot open '%s'\n", argv[optind]);
            return 1;
        }
    }

    char prev[MAX_LINE];
    char cur[MAX_LINE];
    int count = 0;
    int has_prev = 0;

    while (fgets(cur, sizeof(cur), fp)) {
        int len = strlen(cur);
        if (len > 0 && cur[len - 1] == '\n') cur[--len] = '\0';

        if (!has_prev) {
            strcpy(prev, cur);
            count = 1;
            has_prev = 1;
        } else if (strcmp(prev, cur) == 0) {
            count++;
        } else {
            if (count_flag)
                printf("%7d %s\n", count, prev);
            else
                printf("%s\n", prev);
            strcpy(prev, cur);
            count = 1;
        }
    }

    if (has_prev) {
        if (count_flag)
            printf("%7d %s\n", count, prev);
        else
            printf("%s\n", prev);
    }

    if (fp != stdin) fclose(fp);
    return 0;
}
