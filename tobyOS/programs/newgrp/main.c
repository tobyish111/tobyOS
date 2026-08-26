/* newgrp/main.c -- POSIX XCU `newgrp`: change the real/effective group id
 * and replace this process with a new shell.
 *
 *   newgrp [-l | -] [group]
 *
 * With no operand the target is the caller's login gid from /etc/passwd.
 * A named operand is looked up in /etc/group; an all-digit operand that
 * names no group is accepted as a raw gid only if some group HAS that gid,
 * the way shadow's newgrp behaves.
 *
 * Permission: uid 0 may switch anywhere; anyone else may switch to their
 * login group or to a group whose member list names them. Group passwords
 * are NOT supported -- a non-member is refused outright, which is the
 * behaviour of the password-less common case everywhere.
 *
 * On success the process becomes $SHELL (else the passwd shell, else
 * /bin/sh); -l puts the conventional `-` on argv[0] so the shell starts as
 * a login shell. On any failure the shell that invoked us is still there,
 * and the exit status is 1, per POSIX ">0".
 *
 * The native ABI has no setgroups, so the supplementary set cannot be
 * reinitialised here; the primary gid is the whole story.
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

static int pw_lookup_uid(long uid, struct pwent *out) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    char line[LINE_CAP];
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        char *fld[8];
        if (split_colon(line, fld, 8) < 7) continue;
        if (atol(fld[2]) == uid) {
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

/* Find a group by name, or -- for an all-digit operand -- by gid.
 * Fills *gid_out and *is_member (whether `user` is in the member list). */
static int grp_lookup(const char *arg, const char *user,
                      long *gid_out, int *is_member) {
    int numeric = 1;
    for (const char *p = arg; *p; p++)
        if (*p < '0' || *p > '9') { numeric = 0; break; }
    if (!*arg) numeric = 0;

    FILE *f = fopen("/etc/group", "r");
    if (!f) return -1;
    char line[LINE_CAP];
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        char *fld[8];
        int nf = split_colon(line, fld, 8);
        if (nf < 3) continue;
        if (strcmp(fld[0], arg) == 0 ||
            (numeric && atol(fld[2]) == atol(arg))) {
            *gid_out = atol(fld[2]);
            *is_member = (nf >= 4 && member_list_has(fld[3], user));
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

int main(int argc, char **argv) {
    int login = 0;
    const char *grp_arg = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-") == 0) {
            login = 1;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "usage: newgrp [-l] [group]\n");
            return 1;
        } else if (!grp_arg) {
            grp_arg = argv[i];
        } else {
            fprintf(stderr, "usage: newgrp [-l] [group]\n");
            return 1;
        }
    }

    long uid = (long)getuid();
    struct pwent pw;
    if (pw_lookup_uid(uid, &pw) != 0) {
        fprintf(stderr, "newgrp: uid %ld not in /etc/passwd\n", uid);
        return 1;
    }

    long target;
    if (grp_arg) {
        int is_member = 0;
        if (grp_lookup(grp_arg, pw.name, &target, &is_member) != 0) {
            fprintf(stderr, "newgrp: group '%s' does not exist\n", grp_arg);
            return 1;
        }
        if (uid != 0 && target != pw.gid && !is_member) {
            fprintf(stderr, "newgrp: Permission denied\n");
            return 1;
        }
    } else {
        target = pw.gid;      /* no operand: back to the login group */
    }

    if (setgid((gid_t)target) != 0) {
        fprintf(stderr, "newgrp: setgid(%ld) failed\n", target);
        return 1;
    }

    const char *shell = getenv("SHELL");
    if (!shell || !*shell) shell = pw.shell;
    if (!shell || !*shell) shell = "/bin/sh";

    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    char argv0[144];
    snprintf(argv0, sizeof argv0, "%s%s", login ? "-" : "", base);

    char *sh_argv[2];
    sh_argv[0] = argv0;
    sh_argv[1] = 0;
    execve(shell, sh_argv, environ);
    fprintf(stderr, "newgrp: exec '%s' failed\n", shell);
    return 1;
}
