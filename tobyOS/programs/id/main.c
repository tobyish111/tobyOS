/* id/main.c -- POSIX `id`: report user and group identity.
 *
 *   id [-Ggu] [-nr] [user]
 *
 * With a USER operand the answer comes from the DATABASE (/etc/passwd for
 * uid and login gid, /etc/group for the groups that list the user as a
 * member). With no operand it describes the CALLING PROCESS: this kernel's
 * native ABI has no getgroups, so the process form reports the primary gid
 * as the whole `groups=` list -- the honest answer, since a supplementary
 * set the kernel cannot report cannot be claimed either.
 *
 * Names come from the database; an id with no entry prints bare numbers,
 * the way coreutils does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LINE_CAP 512

struct pwent {
    char name[64];
    long uid, gid;
    char shell[128];
};

/* Split a colon-separated line in place. Returns the field count. */
static int split_colon(char *line, char *fields[], int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *c = strchr(p, ':');
        if (!c) break;
        *c = '\0';
        p = c + 1;
    }
    return n;
}

static void chomp(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

/* Look a user up by name (name != 0) or by uid. */
static int pw_lookup(const char *name, long uid, struct pwent *out) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    char line[LINE_CAP];
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        char *fld[8];
        if (split_colon(line, fld, 8) < 7) continue;
        if (name ? (strcmp(fld[0], name) == 0) : (atol(fld[2]) == uid)) {
            snprintf(out->name, sizeof out->name, "%s", fld[0]);
            out->uid = atol(fld[2]);
            out->gid = atol(fld[3]);
            snprintf(out->shell, sizeof out->shell, "%s", fld[6]);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static int grp_name_of(long gid, char *out, size_t cap) {
    FILE *f = fopen("/etc/group", "r");
    if (!f) return -1;
    char line[LINE_CAP];
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        char *fld[8];
        if (split_colon(line, fld, 8) < 3) continue;
        if (atol(fld[2]) == gid) {
            snprintf(out, cap, "%s", fld[0]);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static int member_list_has(const char *list, const char *user) {
    size_t ul = strlen(user);
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == ul && memcmp(p, user, ul) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* The user's groups from the database: login gid first, then every group
 * whose member list names the user. Returns the count. */
static int db_groups(const char *user, long login_gid, long *gids, int cap) {
    int n = 0;
    if (n < cap) gids[n++] = login_gid;
    FILE *f = fopen("/etc/group", "r");
    if (!f) return n;
    char line[LINE_CAP];
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        char *fld[8];
        int nf = split_colon(line, fld, 8);
        if (nf < 3) continue;
        long gid = atol(fld[2]);
        if (gid == login_gid) continue;
        if (nf >= 4 && member_list_has(fld[3], user) && n < cap)
            gids[n++] = gid;
    }
    fclose(f);
    return n;
}

static void print_id(long id, const char *name, int names_only) {
    if (names_only) {
        if (name) printf("%s", name);
        else printf("%ld", id);
    } else {
        printf("%ld", id);
    }
}

static void usage(void) {
    fprintf(stderr, "usage: id [-Ggu] [-nr] [user]\n");
}

int main(int argc, char **argv) {
    int want_G = 0, want_g = 0, want_u = 0;
    int names = 0, real = 0;
    const char *user = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *p = argv[i] + 1; *p; p++) {
                switch (*p) {
                case 'G': want_G = 1; break;
                case 'g': want_g = 1; break;
                case 'u': want_u = 1; break;
                case 'n': names  = 1; break;
                case 'r': real   = 1; break;
                default: usage(); return 1;
                }
            }
        } else if (!user) {
            user = argv[i];
        } else {
            usage();
            return 1;
        }
    }
    if (want_G + want_g + want_u > 1) { usage(); return 1; }
    (void)real;   /* the native ABI keeps one uid/gid pair; -r == effective */

    struct pwent pw;
    long uid, gid;
    long gids[32];
    int ngids;
    int have_pw;

    if (user) {
        if (pw_lookup(user, 0, &pw) != 0) {
            fprintf(stderr, "id: '%s': no such user\n", user);
            return 1;
        }
        uid = pw.uid;
        gid = pw.gid;
        have_pw = 1;
        ngids = db_groups(pw.name, pw.gid, gids, 32);
    } else {
        uid = (long)geteuid();
        gid = (long)getegid();
        have_pw = (pw_lookup(0, uid, &pw) == 0);
        gids[0] = gid;
        ngids = 1;
    }

    char uname[64], gname[64];
    const char *un = (have_pw ? pw.name : 0);
    const char *gn = (grp_name_of(gid, gname, sizeof gname) == 0 ? gname : 0);
    (void)uname;

    if (want_u) { print_id(uid, un, names); printf("\n"); return 0; }
    if (want_g) { print_id(gid, gn, names); printf("\n"); return 0; }
    if (want_G) {
        for (int i = 0; i < ngids; i++) {
            char nb[64];
            const char *nm =
                (grp_name_of(gids[i], nb, sizeof nb) == 0 ? nb : 0);
            if (i) printf(" ");
            print_id(gids[i], nm, names);
        }
        printf("\n");
        return 0;
    }

    printf("uid=%ld", uid);
    if (un) printf("(%s)", un);
    printf(" gid=%ld", gid);
    if (gn) printf("(%s)", gn);
    printf(" groups=");
    for (int i = 0; i < ngids; i++) {
        char nb[64];
        const char *nm = (grp_name_of(gids[i], nb, sizeof nb) == 0 ? nb : 0);
        if (i) printf(",");
        printf("%ld", gids[i]);
        if (nm) printf("(%s)", nm);
    }
    printf("\n");
    return 0;
}
