/* less/main.c -- Terminal pager for tobyOS.
 *
 * Reads a file (or stdin) and displays one screenful at a time.
 * Navigation: space/PgDn, b/PgUp, j/k, g/G, /pattern search, n/N, q.
 * Line numbers with -N, as Linux's less; position and percentage in the
 * status bar.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>

/* The window's size, as tvi asks for it (src/tty.c, src/pty.c answer). */
#define TIOCGWINSZ 0x5413u
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

#define MAX_LINES    65536
#define MAX_LINE_LEN 4096
#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80

static char *g_lines[MAX_LINES];
static int g_nlines;
static int g_top;
static int g_is_more;   /* invoked as `more`: cat when piped, stop at EOF */
static int g_rows = DEFAULT_ROWS;
static int g_cols = DEFAULT_COLS;
static char g_search[256];
static int g_search_dir = 1;
static const char *g_filename = "(stdin)";
/* -N: number the lines. Linux's less numbers them only when asked; this one
 * numbered every line, whatever it was given -- `seq 100 | less` showed
 * "     1 1" -- and -N, parsed to turn them OFF, was then thrown away. */
static int g_show_lines;

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

/* KEYS COME FROM THE TERMINAL, NOT FROM STANDARD INPUT. `cmd | less` has
 * the pipe there, already read to its end by load_stdin, so the first
 * read for a key said EOF and the pager quit after one screen -- the keys
 * typed then went to the shell (`seq 100 | less` in the desktop's terminal
 * window, then `q`: "qecho: command not found"; tools/agent/probes/b21
 * lesspty.py). Linux less reads its keys from /dev/tty for this; failing
 * that, stderr, as the classic more did, when that is the terminal. And
 * the terminal is put in raw mode -- a key at a time, not echoed, ^C a
 * key -- and back as it was when the pager ends: it was left cooked, so a
 * key waited for Enter and echoed over the screen. */
static int g_kfd = -1;
static struct termios g_saved_tio;
static struct termios g_raw_tio;
static int g_raw;
/* -K: ^C quits (status 2). Without it less only rings the bell, as
 * Linux's does; more quits (status 0). */
static int g_quit_on_intr;

static void term_restore(void) {
    if (g_raw) tcsetattr(g_kfd, TCSANOW, &g_saved_tio);
    g_raw = 0;
}

static int term_open(void) {
    if (isatty(STDIN_FILENO)) g_kfd = STDIN_FILENO;
    else {
        g_kfd = open("/dev/tty", O_RDONLY);
        if (g_kfd < 0 && isatty(STDERR_FILENO)) g_kfd = STDERR_FILENO;
    }
    if (g_kfd < 0) return -1;
    if (tcgetattr(g_kfd, &g_saved_tio) == 0) {
        struct termios t = g_saved_tio;
        t.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(g_kfd, TCSANOW, &t) == 0) { g_raw = 1; g_raw_tio = t; }
    }
    /* the window's size, not a fixed 24x80 -- no wider than a line's
     * buffer (draw_screen cuts each line at g_cols in one) */
    struct winsize ws;
    if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 ||
         ioctl(g_kfd, TIOCGWINSZ, &ws) == 0) && ws.ws_row > 2 && ws.ws_col > 10) {
        g_rows = ws.ws_row;
        g_cols = ws.ws_col < MAX_LINE_LEN ? ws.ws_col : MAX_LINE_LEN - 1;
    }
    return 0;
}

static int term_readch(void) {
    unsigned char c;
    if (read(g_kfd, &c, 1) == 1) return c;
    return -1;
}

/* Is C the terminal's control character IDX (VINTR, VSUSP)? Raw mode turned
 * ISIG off, so ^C and ^Z come as keys: the pager does what the terminal
 * would have, as Linux's does (tools/agent/probes/b21/lessintr-linux.py,
 * on a pty: less rings the bell on ^C and stays, less -K and more quit; ^Z
 * stops the job either way, and `fg` brings the pager back). */
static int term_is_cc(int c, int idx) {
    return g_raw && g_saved_tio.c_cc[idx] && c == g_saved_tio.c_cc[idx];
}

static void draw_screen(void);

/* ^Z: the terminal as the shell had it, the job -- the caller's process
 * group, the whole pipeline -- stopped, as the terminal's SIGTSTP stops it;
 * and on `fg`, raw again and the screen drawn again. */
static void term_suspend(void) {
    term_move(g_rows, 1);
    term_write("\033[K");
    tcsetattr(g_kfd, TCSANOW, &g_saved_tio);
    kill(0, SIGTSTP);
    tcsetattr(g_kfd, TCSANOW, &g_raw_tio);
    draw_screen();
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
            int maxw = g_cols;
            if (g_show_lines) {
                char num[16];
                snprintf(num, sizeof(num), "%6d ", idx + 1);
                term_write(num);
                maxw = g_cols - 8;
            }

            const char *line = g_lines[idx];
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
        if (c == 27 || c == -1 || term_is_cc(c, VINTR)) { len = 0; break; }
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
    const char *base = argv[0] ? argv[0] : "less";
    for (const char *q = base; *q; q++) if (*q == 47) base = q + 1;
    g_is_more = !strcmp(base, "more");
    int start_line = 0;

    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!g_is_more && (strcmp(argv[i], "-N") == 0 ||
                           strcmp(argv[i], "--LINE-NUMBERS") == 0))
            g_show_lines = 1;
        else if (!g_is_more && (strcmp(argv[i], "-n") == 0 ||
                                strcmp(argv[i], "--line-numbers") == 0))
            g_show_lines = 0;
        else if (!g_is_more && (strcmp(argv[i], "-K") == 0 ||
                                strcmp(argv[i], "--quit-on-intr") == 0))
            g_quit_on_intr = 1;
        else if (argv[i][0] == '+' && argv[i][1] >= '0' && argv[i][1] <= '9')
            start_line = atoi(argv[i] + 1) - 1;   /* more +N starts at line N */
        else if (argv[i][0] != '-') path = argv[i];
    }

    if (path) {
        if (load_file(path) != 0) return 1;
    } else {
        load_stdin();
    }

    if (g_nlines == 0) return 0;
    if (start_line > 0) g_top = start_line < g_nlines ? start_line : g_nlines - 1;

    /* Not a terminal: the input is copied through, which is what makes
     * `cmd | more > file` and `less f | grep x` behave at all -- less does
     * it too, as Linux's does. So with no terminal to read keys from. */
    if (!isatty(1) || term_open() != 0) {
        for (int k = g_top; k < g_nlines; k++)
            printf("%s\n", g_lines[k]);
        return 0;
    }

    int visible = g_rows - 1;
    if (g_nlines - g_top <= visible) {
        term_restore();
        for (int k = g_top; k < g_nlines; k++)
            printf("%s\n", g_lines[k]);
        return 0;
    }

    draw_screen();

    int running = 1, status = 0;
    while (running) {
        int c = term_readch();
        if (c == -1) break;

        if (term_is_cc(c, VINTR)) {
            if (g_is_more) break;
            term_write("\a");
            if (g_quit_on_intr) { status = 2; break; }
            continue;
        }
        if (term_is_cc(c, VSUSP)) {
            term_suspend();
            continue;
        }

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
    term_restore();
    return status;
}
