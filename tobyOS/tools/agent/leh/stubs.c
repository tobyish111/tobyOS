/* stubs.c -- run programs/tsh/lineedit.c on Linux (WSL) under a pty, with
 * the shell's side faked: a fixed history, no completion. Prints each line
 * the editor returns between markers, so a driver can compare it with what
 * bash's readline makes of the same keystrokes. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lineedit.h"

static const char *hist[16] = {
    "# first entry one two",
    "# echo alpha beta gamma",
    "# ls /usr/lib/x.tar.gz foo",
};
static int nhist = 3;
static char *added[256];
static int nadded;

bool tsh_progcomp(const char *line, size_t point, const char *word,
                  char ***out, int *nout, unsigned long *opts) {
    (void)line; (void)point; (void)word; (void)out; (void)nout; (void)opts;
    return false;
}
void tsh_progcomp_free(char **v, int n) { (void)v; (void)n; }
const char *tsh_builtin_name_at(int i) { (void)i; return 0; }
const char *shell_get_var_hosted(const char *name) { return getenv(name); }
int shell_history_count_hosted(void) { return nhist + nadded; }
const char *shell_history_get_hosted(int i) {
    if (i < 0) return 0;
    if (i < nhist) return hist[i];
    if (i - nhist < nadded) return added[i - nhist];
    return 0;
}
bool shell_utf8_hosted(void) { return true; }
static bool vi_on;
bool shell_opt_vi_hosted(void) { return vi_on; }
void shell_set_editing_mode_hosted(bool vi) { vi_on = vi; }
unsigned long shell_history_base_hosted(void) { return 0; }

int main(int argc, char **argv) {
    vi_on = argc > 1 && strcmp(argv[1], "vi") == 0;
    /* LEH_HIST: the history instead, entries separated by newlines */
    const char *eh = getenv("LEH_HIST");
    if (eh) {
        char *c = strdup(eh), *sv = 0;
        nhist = 0;
        for (char *t = strtok_r(c, "\n", &sv); t && nhist < 16; t = strtok_r(0, "\n", &sv))
            hist[nhist++] = t;
    }
    char *buf = 0;
    size_t cap = 0;
    for (;;) {
        long r = tsh_readline("$ ", &buf, &cap);
        if (r == TSH_LE_EOF) { printf("<<EOF>>\n"); break; }
        if (r == TSH_LE_INTR) { printf("<<INTR>>\n"); fflush(stdout); continue; }
        printf("<<LINE:%s>>\n", buf);
        fflush(stdout);
        if (nadded < 256 && buf[0]) added[nadded++] = strdup(buf);
    }
    return 0;
}

volatile int g_tsh_async_ok, g_tsh_le_async;
