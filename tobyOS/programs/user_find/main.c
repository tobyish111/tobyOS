#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static int glob_match(const char *pat, const char *str) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (; *str; str++) {
                if (glob_match(pat, str)) return 1;
            }
            return 0;
        } else if (*pat == '?') {
            if (!*str) return 0;
            pat++; str++;
        } else {
            if (*pat != *str) return 0;
            pat++; str++;
        }
    }
    return *str == '\0';
}

static void find_recurse(const char *dir, const char *pattern) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        if (!pattern || glob_match(pattern, ent->d_name))
            printf("%s\n", path);

        struct stat st;
        if (stat(path, &st) == 0 && (st.st_mode & S_IFDIR))
            find_recurse(path, pattern);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    const char *start = ".";
    const char *pattern = NULL;

    int i = 1;
    if (i < argc && argv[i][0] != '-') {
        start = argv[i++];
    }

    while (i < argc) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            pattern = argv[i + 1];
            i += 2;
        } else {
            fprintf(stderr, "find: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    find_recurse(start, pattern);
    return 0;
}
