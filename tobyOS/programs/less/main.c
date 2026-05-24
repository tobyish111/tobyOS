/* less/main.c -- Terminal pager for tobyOS.
 *
 * Reads a file (or stdin) and displays one screenful at a time.
 * Navigation: space/PgDn, b/PgUp, j/k, g/G, /pattern search, n/N, q.
 * Shows line numbers and percentage indicator in status bar.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_LINES    65536
#define MAX_LINE_LEN 4096
#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80

static char *g_lines[MAX_LINES];
static int g_nlines;
static int g_top;
static int g_rows = DEFAULT_ROWS;
static int g_cols = DEFAULT_COLS;
static char g_search[256];
static int g_search_dir = 1;
static const char *g_filename = "(stdin)";

/* ---- Terminal raw I/O ------------------------------------------------ */

static void term_write(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
}

static void term_clear(void) {
    term_write("\033[2J\033[H");
}

static void term_move(int row, int col) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\033[%d;%dH", row, col);
    term_write(buf);
}

static void term_reverse(int on) {
    term_write(on ? "\033[7m" : "\033[0m");
}

static int term_readch(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1) return c;
    return -1;
}

/* ---- File loading --------------------------------------------------- */

static int load_file(const char *path) {
    FILE *f = NULL;
    if (path) {
        f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "less: %s: No such file or directory\n", path);
            return -1;
        }
        g_filename = path;
    } else {
        f = stdin;
    }

    char buf[MAX_LINE_LEN];
    while (g_nlines < MAX_LINES && fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
        if (len > 0 && buf[len-1] == '\r') buf[--len] = '\0';
        g_lines[g_nlines] = strdup(buf);
        g_nlines++;
    }

    if (path && f) fclose(f);
    return 0;
}

static int load_stdin(void) {
    char buf[MAX_LINE_LEN];
    while (g_nlines < MAX_LINES && fgets(buf, sizeof(buf), stdin)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
        if (len > 0 && buf[len-1] == '\r') buf[--len] = '\0';
        g_lines[g_nlines] = strdup(buf);
        g_nlines++;
    }
    return 0;
}

/* ---- Simple substring search ---------------------------------------- */

static int str_search(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

/* ---- Display -------------------------------------------------------- */

static void draw_screen(void) {
    term_clear();
    int visible = g_rows - 1;
    for (int i = 0; i < visible; i++) {
        int idx = g_top + i;
        if (idx < g_nlines) {
            char num[16];
            snprintf(num, sizeof(num), "%6d ", idx + 1);
            term_write(num);

            const char *line = g_lines[idx];
            int maxw = g_cols - 8;
            if (maxw < 1) maxw = 1;
            char truncated[MAX_LINE_LEN];
            strncpy(truncated, line, maxw);
            truncated[maxw] = '\0';

            if (g_search[0] && str_search(line, g_search)) {
                term_reverse(1);
                term_write(truncated);
                term_reverse(0);
            } else {
                term_write(truncated);
            }
        } else {
            term_write("~");
        }
        term_write("\r\n");
    }

    /* Status bar */
    term_move(g_rows, 1);
    term_reverse(1);
    char status[256];
    int pct = g_nlines > 0 ? (g_top + visible) * 100 / g_nlines : 100;
    if (pct > 100) pct = 100;
    if (g_top == 0 && g_nlines <= visible)
        snprintf(status, sizeof(status), " %s (lines %d) -- (END)",
                 g_filename, g_nlines);
    else if (g_top == 0)
        snprintf(status, sizeof(status), " %s (lines %d) -- top",
                 g_filename, g_nlines);
    else
        snprintf(status, sizeof(status), " %s  line %d/%d  %d%%",
                 g_filename, g_top + 1, g_nlines, pct);

    int slen = (int)strlen(status);
    term_write(status);
    for (int i = slen; i < g_cols; i++) term_write(" ");
    term_reverse(0);
}

/* ---- Search --------------------------------------------------------- */

static int search_forward(int from) {
    for (int i = from + 1; i < g_nlines; i++)
        if (str_search(g_lines[i], g_search)) return i;
    for (int i = 0; i <= from; i++)
        if (str_search(g_lines[i], g_search)) return i;
    return -1;
}

static int search_backward(int from) {
    for (int i = from - 1; i >= 0; i--)
        if (str_search(g_lines[i], g_search)) return i;
    for (int i = g_nlines - 1; i >= from; i--)
        if (str_search(g_lines[i], g_search)) return i;
    return -1;
}

static void prompt_search(void) {
    int visible = g_rows - 1;
    (void)visible;
    term_move(g_rows, 1);
    term_reverse(1);
    term_write("/");
    for (int i = 1; i < g_cols; i++) term_write(" ");
    term_move(g_rows, 2);
    term_reverse(0);

    char buf[256];
    int len = 0;
    while (1) {
        int c = term_readch();
        if (c == '\n' || c == '\r') break;
        if (c == 27 || c == -1) { len = 0; break; }
        if (c == 127 || c == 8) {
            if (len > 0) len--;
        } else if (len < 255) {
            buf[len++] = (char)c;
        }
        buf[len] = '\0';
        term_move(g_rows, 2);
        term_write(buf);
        term_write(" ");
    }
    buf[len] = '\0';
    if (len > 0) {
        strncpy(g_search, buf, 255);
        g_search[255] = '\0';
        g_search_dir = 1;
    }
}

/* ---- Main loop ------------------------------------------------------ */

static void clamp_top(void) {
    int visible = g_rows - 1;
    int max_top = g_nlines - visible;
    if (max_top < 0) max_top = 0;
    if (g_top > max_top) g_top = max_top;
    if (g_top < 0) g_top = 0;
}

int main(int argc, char **argv) {
    int show_lines = 1;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-N") == 0) show_lines = 0;
        else if (argv[i][0] != '-') path = argv[i];
    }
    (void)show_lines;

    if (path) {
        if (load_file(path) != 0) return 1;
    } else {
        load_stdin();
    }

    if (g_nlines == 0) return 0;

    int visible = g_rows - 1;
    if (g_nlines <= visible) {
        for (int i = 0; i < g_nlines; i++)
            printf("%s\n", g_lines[i]);
        return 0;
    }

    draw_screen();

    int running = 1;
    while (running) {
        int c = term_readch();
        if (c == -1) break;

        /* Handle escape sequences for arrow keys / pgup / pgdn */
        if (c == 27) {
            int c2 = term_readch();
            if (c2 == '[') {
                int c3 = term_readch();
                switch (c3) {
                case 'A': c = 'k'; break; /* up */
                case 'B': c = 'j'; break; /* down */
                case '5': term_readch(); c = 'b'; break; /* pgup */
                case '6': term_readch(); c = ' '; break; /* pgdn */
                default: continue;
                }
            } else continue;
        }

        switch (c) {
        case 'q': case 'Q':
            running = 0;
            break;
        case ' ': case 'f':
            g_top += visible;
            clamp_top();
            break;
        case 'b':
            g_top -= visible;
            clamp_top();
            break;
        case 'j': case '\n':
            g_top++;
            clamp_top();
            break;
        case 'k':
            g_top--;
            clamp_top();
            break;
        case 'd':
            g_top += visible / 2;
            clamp_top();
            break;
        case 'u':
            g_top -= visible / 2;
            clamp_top();
            break;
        case 'g':
            g_top = 0;
            break;
        case 'G':
            g_top = g_nlines - visible;
            clamp_top();
            break;
        case '/':
            prompt_search();
            if (g_search[0]) {
                int found = search_forward(g_top);
                if (found >= 0) {
                    g_top = found;
                    clamp_top();
                }
            }
            break;
        case 'n':
            if (g_search[0]) {
                int found = (g_search_dir > 0)
                    ? search_forward(g_top) : search_backward(g_top);
                if (found >= 0) { g_top = found; clamp_top(); }
            }
            break;
        case 'N':
            if (g_search[0]) {
                int found = (g_search_dir > 0)
                    ? search_backward(g_top) : search_forward(g_top);
                if (found >= 0) { g_top = found; clamp_top(); }
            }
            break;
        }
        draw_screen();
    }

    term_clear();
    return 0;
}
