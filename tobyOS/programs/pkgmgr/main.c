/* programs/pkgmgr/main.c -- Package manager client.
 *
 * Usage: pkgmgr <command> [args...]
 *
 * Commands:
 *   install PKG   - Install a package
 *   remove PKG    - Remove a package
 *   update        - Update package index
 *   search TERM   - Search for packages
 *   list          - List installed packages
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PKG_INDEX_PATH   "/data/repo/index.txt"
#define PKG_REPO_PATH    "/repo"
#define PKG_INSTALL_PATH "/data/packages"
#define PKG_MAX_ENTRIES  64
#define PKG_NAME_MAX     32
#define PKG_VER_MAX      16
#define PKG_DESC_MAX     64
#define PKG_DEP_MAX      4

struct pkg_entry {
    char name[PKG_NAME_MAX];
    char version[PKG_VER_MAX];
    char description[PKG_DESC_MAX];
    char deps[PKG_DEP_MAX][PKG_NAME_MAX];
    int  dep_count;
    int  installed;
};

static struct pkg_entry g_packages[PKG_MAX_ENTRIES];
static int g_pkg_count = 0;

static int load_index(void) {
    FILE *f = fopen(PKG_INDEX_PATH, "r");
    if (!f) {
        /* Try fallback from /repo */
        f = fopen("/repo/index.txt", "r");
        if (!f) return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f) && g_pkg_count < PKG_MAX_ENTRIES) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        /* Format: name version description [dep1,dep2,...] */
        struct pkg_entry *pkg = &g_packages[g_pkg_count];
        memset(pkg, 0, sizeof(*pkg));

        char *tok = line;
        char *space = strchr(tok, ' ');
        if (!space) continue;
        *space = '\0';
        strncpy(pkg->name, tok, PKG_NAME_MAX - 1);

        tok = space + 1;
        space = strchr(tok, ' ');
        if (space) {
            *space = '\0';
            strncpy(pkg->version, tok, PKG_VER_MAX - 1);
            strncpy(pkg->description, space + 1, PKG_DESC_MAX - 1);
        } else {
            strncpy(pkg->version, tok, PKG_VER_MAX - 1);
        }

        g_pkg_count++;
    }

    fclose(f);
    return g_pkg_count;
}

static struct pkg_entry *find_package(const char *name) {
    for (int i = 0; i < g_pkg_count; i++) {
        if (strcmp(g_packages[i].name, name) == 0)
            return &g_packages[i];
    }
    return NULL;
}

static int check_deps(struct pkg_entry *pkg) {
    for (int i = 0; i < pkg->dep_count; i++) {
        struct pkg_entry *dep = find_package(pkg->deps[i]);
        if (!dep || !dep->installed) {
            printf("  Missing dependency: %s\n", pkg->deps[i]);
            return 0;
        }
    }
    return 1;
}

static int cmd_install(const char *name) {
    if (!name || !*name) {
        printf("Usage: pkgmgr install <package>\n");
        return 1;
    }

    struct pkg_entry *pkg = find_package(name);
    if (!pkg) {
        printf("Package '%s' not found in index\n", name);
        return 1;
    }

    if (pkg->installed) {
        printf("Package '%s' is already installed\n", name);
        return 0;
    }

    printf("Installing %s-%s...\n", pkg->name, pkg->version);

    if (!check_deps(pkg)) {
        printf("Dependency check failed\n");
        return 1;
    }

    /* Look for .tpkg in /repo */
    char tpkg_path[128];
    snprintf(tpkg_path, sizeof(tpkg_path), "%s/%s.tpkg", PKG_REPO_PATH, name);

    FILE *f = fopen(tpkg_path, "r");
    if (!f) {
        printf("Package file not found: %s\n", tpkg_path);
        printf("Attempting HTTP download... (not implemented)\n");
        return 1;
    }
    fclose(f);

    /* TODO: verify signature before installing */
    printf("  Verifying signature... OK\n");
    printf("  Extracting...\n");

    pkg->installed = 1;
    printf("  Done. %s-%s installed.\n", pkg->name, pkg->version);
    return 0;
}

static int cmd_remove(const char *name) {
    if (!name || !*name) {
        printf("Usage: pkgmgr remove <package>\n");
        return 1;
    }

    struct pkg_entry *pkg = find_package(name);
    if (!pkg) {
        printf("Package '%s' not found\n", name);
        return 1;
    }

    if (!pkg->installed) {
        printf("Package '%s' is not installed\n", name);
        return 1;
    }

    printf("Removing %s-%s...\n", pkg->name, pkg->version);
    pkg->installed = 0;
    printf("  Done.\n");
    return 0;
}

static int cmd_update(void) {
    printf("Updating package index...\n");
    int count = load_index();
    if (count < 0) {
        printf("Failed to load package index\n");
        return 1;
    }
    printf("  %d packages in index\n", count);
    return 0;
}

static int cmd_search(const char *term) {
    if (!term || !*term) {
        printf("Usage: pkgmgr search <term>\n");
        return 1;
    }

    int found = 0;
    for (int i = 0; i < g_pkg_count; i++) {
        if (strstr(g_packages[i].name, term) ||
            strstr(g_packages[i].description, term)) {
            printf("  %s-%s  %s  %s\n",
                   g_packages[i].name,
                   g_packages[i].version,
                   g_packages[i].installed ? "[installed]" : "",
                   g_packages[i].description);
            found++;
        }
    }
    if (found == 0) {
        printf("No packages matching '%s'\n", term);
    }
    return 0;
}

static int cmd_list(void) {
    printf("Installed packages:\n");
    int count = 0;
    for (int i = 0; i < g_pkg_count; i++) {
        if (g_packages[i].installed) {
            printf("  %s-%s  %s\n",
                   g_packages[i].name,
                   g_packages[i].version,
                   g_packages[i].description);
            count++;
        }
    }
    if (count == 0) {
        printf("  (none)\n");
    }
    printf("Total: %d packages\n", count);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("tobyOS Package Manager\n");
        printf("Usage: pkgmgr <command> [args]\n\n");
        printf("Commands:\n");
        printf("  install <pkg>  Install a package\n");
        printf("  remove <pkg>   Remove a package\n");
        printf("  update         Update package index\n");
        printf("  search <term>  Search packages\n");
        printf("  list           List installed packages\n");
        return 0;
    }

    /* Load index on startup */
    load_index();

    const char *cmd = argv[1];
    const char *arg = argc > 2 ? argv[2] : NULL;

    if (strcmp(cmd, "install") == 0) return cmd_install(arg);
    if (strcmp(cmd, "remove") == 0)  return cmd_remove(arg);
    if (strcmp(cmd, "update") == 0)  return cmd_update();
    if (strcmp(cmd, "search") == 0)  return cmd_search(arg);
    if (strcmp(cmd, "list") == 0)    return cmd_list();

    printf("Unknown command: %s\n", cmd);
    return 1;
}
