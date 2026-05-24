#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <getopt.h>
#include <errno.h>

static int recursive = 0;
static int verbose = 0;

static int remove_path(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "rm: cannot stat '%s': %s\n", path, strerror(errno));
        return 1;
    }

    if ((st.st_mode & S_IFDIR) && recursive) {
        DIR *d = opendir(path);
        if (!d) {
            fprintf(stderr, "rm: cannot open dir '%s': %s\n", path, strerror(errno));
            return 1;
        }
        struct dirent *ent;
        int err = 0;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            if (remove_path(child) != 0) err = 1;
        }
        closedir(d);
        if (rmdir(path) < 0) {
            fprintf(stderr, "rm: cannot remove dir '%s': %s\n", path, strerror(errno));
            return 1;
        }
        if (verbose) printf("removed directory '%s'\n", path);
        return err;
    } else if (st.st_mode & S_IFDIR) {
        fprintf(stderr, "rm: '%s' is a directory (use -r)\n", path);
        return 1;
    }

    if (unlink(path) < 0) {
        fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (verbose) printf("removed '%s'\n", path);
    return 0;
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "rv")) != -1) {
        switch (opt) {
        case 'r': recursive = 1; break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "usage: rm [-r] [-v] path...\n");
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "usage: rm [-r] [-v] path...\n");
        return 1;
    }

    int ret = 0;
    for (int i = optind; i < argc; i++) {
        if (remove_path(argv[i]) != 0) ret = 1;
    }
    return ret;
}
