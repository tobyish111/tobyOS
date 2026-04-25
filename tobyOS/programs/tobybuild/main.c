/* tobybuild -- M36A: drive /bin/tobycc from a Tobyfile. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static int strcasecmp_toby(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++), cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/m36/Tobyfile";
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    char line[512], out[256] = {0}, src[256] = {0};
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *k = line; while (*k == ' ' || *k == '\t') k++;
        if (!*k) continue;
        char *eq = strchr(k, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
        char *e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) e--;
        *e = 0;
        while (strlen(k) > 0 && (k[strlen(k) - 1] == ' ')) k[strlen(k) - 1] = 0;
        if (strcasecmp_toby(k, "out") == 0) { strncpy(out, v, sizeof out - 1); }
        if (strcasecmp_toby(k, "src") == 0) { strncpy(src, v, sizeof src - 1); }
    }
    fclose(f);
    if (!out[0] || !src[0]) { fprintf(stderr, "tobybuild: need out= and src= in %s\n", path); return 1; }
    char b[500];
    snprintf(b, sizeof b, "/bin/tobycc -o %s %s", out, src);
    int r = system(b);
    if (r < 0) { perror("system"); return 1; }
    if (WIFEXITED(r) && WEXITSTATUS(r) == 0) {
        printf("tobybuild: ok -> %s\n", out);
        return 0;
    }
    fprintf(stderr, "tobybuild: tobycc failed (status=%d)\n", r);
    return 1;
}
