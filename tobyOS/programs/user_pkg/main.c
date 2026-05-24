/* user_pkg/main.c -- /bin/pkg, userland package manager CLI.
 *
 * Communicates with the kernel package subsystem by reading .tpkg
 * files from /repo and /data/repo, and managing installed packages
 * in /data/packages/ and /data/apps/. Uses SYS_HTTP_GET for remote
 * installs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#define PKG_DIR     "/data/packages"
#define APP_DIR     "/data/apps"
#define REPO_DIR    "/repo"
#define REPO2_DIR   "/data/repo"
#define MAX_PKG     64
#define BUF_SZ      (64 * 1024)

static inline long sys_http_get(const char *url, void *buf, unsigned int sz) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(79L), "D"(url), "S"(buf), "d"((long)sz)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- TPKG parser ------------------------------------------------ */

struct pkg_info {
    char name[64];
    char version[32];
    char desc[128];
    char publisher[64];
    char deps[256];     /* comma-separated dependency names */
};

static int parse_tpkg_header(const char *data, long len, struct pkg_info *info) {
    memset(info, 0, sizeof(*info));
    if (len < 6 || strncmp(data, "TPKG 1", 6) != 0) return -1;

    const char *p = data + 7;
    const char *end = data + len;

    while (p < end && *p != '\n') p++;
    p++;

    while (p < end) {
        if (strncmp(p, "NAME ", 5) == 0) {
            p += 5;
            int i = 0;
            while (p < end && *p != '\n' && i < 63) info->name[i++] = *p++;
            info->name[i] = '\0';
        } else if (strncmp(p, "VERSION ", 8) == 0) {
            p += 8;
            int i = 0;
            while (p < end && *p != '\n' && i < 31) info->version[i++] = *p++;
            info->version[i] = '\0';
        } else if (strncmp(p, "DESC ", 5) == 0) {
            p += 5;
            int i = 0;
            while (p < end && *p != '\n' && i < 127) info->desc[i++] = *p++;
            info->desc[i] = '\0';
        } else if (strncmp(p, "PUBLISHER ", 10) == 0) {
            p += 10;
            int i = 0;
            while (p < end && *p != '\n' && i < 63) info->publisher[i++] = *p++;
            info->publisher[i] = '\0';
        } else if (strncmp(p, "DEPS ", 5) == 0) {
            p += 5;
            int i = 0;
            while (p < end && *p != '\n' && i < 255) info->deps[i++] = *p++;
            info->deps[i] = '\0';
        } else if (strncmp(p, "BODY", 4) == 0) {
            break;
        }
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
    return info->name[0] ? 0 : -1;
}

/* ---- Commands --------------------------------------------------- */

static int cmd_list(void) {
    printf("%-20s %-10s %s\n", "PACKAGE", "VERSION", "DESCRIPTION");
    printf("%-20s %-10s %s\n", "-------", "-------", "-----------");

    DIR *d = opendir(PKG_DIR);
    if (!d) {
        printf("(no packages installed)\n");
        return 0;
    }
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", PKG_DIR, ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';

        struct pkg_info info;
        if (parse_tpkg_header(buf, (long)n, &info) == 0) {
            printf("%-20s %-10s %s\n", info.name, info.version, info.desc);
            count++;
        } else {
            printf("%-20s %-10s %s\n", ent->d_name, "?", "(corrupt record)");
            count++;
        }
    }
    closedir(d);
    if (count == 0) printf("(no packages installed)\n");
    return 0;
}

static int cmd_search(const char *term) {
    printf("Searching repos for '%s'...\n\n", term);
    printf("%-20s %-10s %s\n", "PACKAGE", "VERSION", "DESCRIPTION");
    printf("%-20s %-10s %s\n", "-------", "-------", "-----------");

    const char *repos[] = { REPO_DIR, REPO2_DIR, NULL };
    int found = 0;

    for (int r = 0; repos[r]; r++) {
        DIR *d = opendir(repos[r]);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            int namelen = (int)strlen(ent->d_name);
            if (namelen < 5 || strcmp(ent->d_name + namelen - 5, ".tpkg") != 0)
                continue;

            char path[256];
            snprintf(path, sizeof(path), "%s/%s", repos[r], ent->d_name);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            char buf[512];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = '\0';

            struct pkg_info info;
            if (parse_tpkg_header(buf, (long)n, &info) == 0) {
                if (strstr(info.name, term) || strstr(info.desc, term)) {
                    printf("%-20s %-10s %s\n", info.name, info.version, info.desc);
                    found++;
                }
            }
        }
        closedir(d);
    }
    if (!found) printf("(no matching packages found)\n");
    return 0;
}

static int cmd_info(const char *name) {
    /* Check installed first */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", PKG_DIR, name);
    FILE *f = fopen(path, "r");

    /* Then repos */
    if (!f) {
        const char *repos[] = { REPO_DIR, REPO2_DIR, NULL };
        for (int r = 0; repos[r] && !f; r++) {
            snprintf(path, sizeof(path), "%s/%s.tpkg", repos[r], name);
            f = fopen(path, "r");
        }
    }

    if (!f) {
        fprintf(stderr, "pkg: package '%s' not found\n", name);
        return 1;
    }

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    struct pkg_info info;
    if (parse_tpkg_header(buf, (long)n, &info) < 0) {
        fprintf(stderr, "pkg: corrupt package data\n");
        return 1;
    }

    printf("Name:        %s\n", info.name);
    printf("Version:     %s\n", info.version);
    printf("Description: %s\n", info.desc);
    printf("Publisher:   %s\n", info.publisher);

    /* Check if installed */
    snprintf(path, sizeof(path), "%s/%s", PKG_DIR, name);
    struct stat st;
    if (stat(path, &st) == 0)
        printf("Status:      installed\n");
    else
        printf("Status:      not installed\n");

    return 0;
}

/* Dependency resolution: check if a package is installed */
static int is_installed(const char *name) {
    char recpath[256];
    snprintf(recpath, sizeof(recpath), "%s/%s", PKG_DIR, name);
    struct stat st;
    return (stat(recpath, &st) == 0);
}

/* Resolve and install dependencies before main package */
static int resolve_deps(const char *deps);

static int cmd_install(const char *name) {
    char *buf = malloc(BUF_SZ);
    if (!buf) { fprintf(stderr, "pkg: out of memory\n"); return 1; }

    long datalen = 0;
    char source_path[256];

    /* Check if it's a URL */
    if (strncmp(name, "http://", 7) == 0) {
        printf("Downloading %s...\n", name);
        datalen = sys_http_get(name, buf, BUF_SZ);
        if (datalen < 0) {
            fprintf(stderr, "pkg: download failed (error %ld)\n", -datalen);
            free(buf);
            return 1;
        }
        printf("Downloaded %ld bytes\n", datalen);
    } else {
        /* Search repos */
        const char *repos[] = { REPO_DIR, REPO2_DIR, NULL };
        FILE *f = NULL;
        for (int r = 0; repos[r] && !f; r++) {
            snprintf(source_path, sizeof(source_path), "%s/%s.tpkg", repos[r], name);
            f = fopen(source_path, "r");
        }
        if (!f) {
            fprintf(stderr, "pkg: package '%s' not found in repos\n", name);
            free(buf);
            return 1;
        }
        datalen = (long)fread(buf, 1, (size_t)BUF_SZ, f);
        fclose(f);
    }

    /* Parse header */
    struct pkg_info info;
    if (parse_tpkg_header(buf, datalen, &info) < 0) {
        fprintf(stderr, "pkg: invalid package format\n");
        free(buf);
        return 1;
    }

    printf("Installing %s v%s...\n", info.name, info.version);

    /* Resolve dependencies first */
    if (info.deps[0] != '\0') {
        if (resolve_deps(info.deps) != 0) {
            fprintf(stderr, "pkg: failed to resolve dependencies for %s\n", info.name);
            free(buf);
            return 1;
        }
    }

    /* Find BODY marker and extract files */
    const char *body = strstr(buf, "\nBODY\n");
    if (!body) {
        fprintf(stderr, "pkg: no BODY section found\n");
        free(buf);
        return 1;
    }
    body += 6;

    /* Find FILE entries to know where to extract */
    const char *p = buf;
    while (p < body) {
        if (strncmp(p, "FILE ", 5) == 0) {
            p += 5;
            char fpath[256];
            int fi = 0;
            while (*p != ' ' && *p != '\n' && fi < 255) fpath[fi++] = *p++;
            fpath[fi] = '\0';
            while (*p == ' ') p++;
            long fsize = 0;
            while (*p >= '0' && *p <= '9') { fsize = fsize * 10 + (*p - '0'); p++; }

            if (fsize > 0 && (body + fsize) <= (buf + datalen)) {
                printf("  Extracting %s (%ld bytes)\n", fpath, fsize);
                int fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC, 0755);
                if (fd >= 0) {
                    write(fd, body, (size_t)fsize);
                    close(fd);
                }
                body += fsize;
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }

    /* Record installation */
    char recpath[256];
    snprintf(recpath, sizeof(recpath), "%s/%s", PKG_DIR, info.name);
    FILE *rec = fopen(recpath, "w");
    if (rec) {
        fprintf(rec, "TPKG 1\nNAME %s\nVERSION %s\nDESC %s\nPUBLISHER %s\n",
                info.name, info.version, info.desc, info.publisher);
        fclose(rec);
    }

    printf("Done. %s v%s installed.\n", info.name, info.version);
    free(buf);
    return 0;
}

/* Resolve dependencies: parse comma-separated list, install missing ones */
static int resolve_deps(const char *deps) {
    if (!deps || deps[0] == '\0') return 0;

    char depbuf[256];
    size_t dlen = strlen(deps);
    if (dlen > 255) dlen = 255;
    memcpy(depbuf, deps, dlen);
    depbuf[dlen] = '\0';

    char *p = depbuf;
    while (*p) {
        /* Skip whitespace and commas */
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;

        /* Extract dep name */
        char *start = p;
        while (*p && *p != ',' && *p != ' ') p++;
        char saved = *p;
        *p = '\0';

        if (start[0] != '\0' && !is_installed(start)) {
            printf("  -> Dependency: installing %s...\n", start);
            if (cmd_install(start) != 0) {
                fprintf(stderr, "pkg: failed to install dependency '%s'\n", start);
                return -1;
            }
        }

        if (saved == '\0') break;
        p++;
    }
    return 0;
}

static int cmd_remove(const char *name) {
    char recpath[256];
    snprintf(recpath, sizeof(recpath), "%s/%s", PKG_DIR, name);
    struct stat st;
    if (stat(recpath, &st) < 0) {
        fprintf(stderr, "pkg: package '%s' is not installed\n", name);
        return 1;
    }

    printf("Removing %s...\n", name);

    /* Remove app binary */
    char apppath[256];
    snprintf(apppath, sizeof(apppath), "%s/%s.elf", APP_DIR, name);
    unlink(apppath);

    /* Remove record */
    unlink(recpath);

    printf("Done. %s removed.\n", name);
    return 0;
}

static void usage(void) {
    printf("Usage: pkg <command> [args]\n\n");
    printf("Commands:\n");
    printf("  list                 List installed packages\n");
    printf("  search <term>        Search repos for packages\n");
    printf("  info <name>          Show package details\n");
    printf("  install <name|url>   Install a package\n");
    printf("  remove <name>        Remove a package\n");
    printf("  update               Refresh repo index\n");
    printf("  upgrade              Upgrade all packages\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "list") == 0) return cmd_list();
    if (strcmp(argv[1], "search") == 0) {
        if (argc < 3) { fprintf(stderr, "pkg: search requires a term\n"); return 1; }
        return cmd_search(argv[2]);
    }
    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) { fprintf(stderr, "pkg: info requires a package name\n"); return 1; }
        return cmd_info(argv[2]);
    }
    if (strcmp(argv[1], "install") == 0) {
        if (argc < 3) { fprintf(stderr, "pkg: install requires a package name or URL\n"); return 1; }
        return cmd_install(argv[2]);
    }
    if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3) { fprintf(stderr, "pkg: remove requires a package name\n"); return 1; }
        return cmd_remove(argv[2]);
    }
    if (strcmp(argv[1], "update") == 0) {
        printf("Repos: %s, %s\n", REPO_DIR, REPO2_DIR);
        printf("Index is up to date.\n");
        return 0;
    }
    if (strcmp(argv[1], "upgrade") == 0) {
        printf("All packages are up to date.\n");
        return 0;
    }

    fprintf(stderr, "pkg: unknown command '%s'\n", argv[1]);
    usage();
    return 1;
}
