/* programs/sh/main.c -- Real POSIX-like shell for tobyOS (Phase 5).
 *
 * Features: line editing, history, tab completion, signal handling,
 * job control. Parsing and execution delegated to parser.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <errno.h>

/* Forward declarations from parser.c */
extern void parser_init(void);
extern int  parser_execute_line(const char *line);
extern int  parser_execute_file(const char *path);
extern int  shell_get_last_status(void);
extern int  shell_is_running(void);
extern void shell_set_interactive(int v);
extern void shell_set_pid(pid_t p);

/* ---- Configuration ---- */

#define MAX_LINE     2048
#define MAX_HISTORY  100
#define MAX_COMPLETE 128
#define PATH_MAX_SH  256

/* ---- History ---- */

static char g_history[MAX_HISTORY][MAX_LINE];
static int  g_hist_count = 0;
static int  g_hist_pos   = 0;

static void hist_add(const char *line) {
    if (!line || !line[0]) return;
    if (g_hist_count > 0 &&
        strcmp(g_history[(g_hist_count - 1) % MAX_HISTORY], line) == 0)
        return;
    strncpy(g_history[g_hist_count % MAX_HISTORY], line, MAX_LINE - 1);
    g_history[g_hist_count % MAX_HISTORY][MAX_LINE - 1] = '\0';
    g_hist_count++;
}

static const char *hist_get(int idx) {
    if (idx < 0 || idx >= g_hist_count) return NULL;
    int oldest = (g_hist_count > MAX_HISTORY) ? g_hist_count - MAX_HISTORY : 0;
    if (idx < oldest) return NULL;
    return g_history[idx % MAX_HISTORY];
}

/* ---- Terminal raw mode helpers ---- */

static int raw_mode = 0;

static void term_raw_enable(void) {
    /* On tobyOS the terminal driver supports a basic raw-mode ioctl.
     * If unavailable, we fall back to line-at-a-time. For now we just
     * set a flag so readline knows to handle char-by-char input. */
    raw_mode = 1;
}

static void term_raw_disable(void) {
    raw_mode = 0;
}

/* ---- Tab completion ---- */

static int complete_commands(const char *prefix, char results[][PATH_MAX_SH], int max) {
    int count = 0;
    int plen = (int)strlen(prefix);

    DIR *d = opendir("/bin");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && count < max) {
            if (strncmp(ent->d_name, prefix, plen) == 0) {
                strncpy(results[count], ent->d_name, PATH_MAX_SH - 1);
                results[count][PATH_MAX_SH - 1] = '\0';
                count++;
            }
        }
        closedir(d);
    }
    return count;
}

static int complete_files(const char *prefix, char results[][PATH_MAX_SH], int max) {
    int count = 0;
    int plen = (int)strlen(prefix);

    const char *slash = NULL;
    for (int i = plen - 1; i >= 0; i--) {
        if (prefix[i] == '/') { slash = &prefix[i]; break; }
    }

    char dir_path[PATH_MAX_SH];
    const char *file_prefix;
    int flen;

    if (slash) {
        int dlen = (int)(slash - prefix) + 1;
        if (dlen >= PATH_MAX_SH) dlen = PATH_MAX_SH - 1;
        memcpy(dir_path, prefix, dlen);
        dir_path[dlen] = '\0';
        file_prefix = slash + 1;
        flen = plen - dlen;
    } else {
        strcpy(dir_path, ".");
        file_prefix = prefix;
        flen = plen;
    }

    DIR *d = opendir(dir_path);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && count < max) {
            if (ent->d_name[0] == '.' && file_prefix[0] != '.') continue;
            if (strncmp(ent->d_name, file_prefix, flen) == 0) {
                if (slash) {
                    int dlen2 = (int)(slash - prefix) + 1;
                    memcpy(results[count], prefix, dlen2);
                    strncpy(results[count] + dlen2, ent->d_name, PATH_MAX_SH - dlen2 - 1);
                } else {
                    strncpy(results[count], ent->d_name, PATH_MAX_SH - 1);
                }
                results[count][PATH_MAX_SH - 1] = '\0';
                count++;
            }
        }
        closedir(d);
    }
    return count;
}

/* ---- Line editing (readline-lite) ---- */

static int read_char(void) {
    char c;
    int r = (int)read(0, &c, 1);
    if (r <= 0) return -1;
    return (unsigned char)c;
}

static void write_str(const char *s) {
    write(1, s, strlen(s));
}

static void write_char(char c) {
    write(1, &c, 1);
}

static void redraw_line(const char *prompt, const char *buf, int len, int cursor) {
    write_str("\r");
    write_str(prompt);
    write(1, buf, len);
    /* Clear any leftover chars */
    write_str("  \r");
    write_str(prompt);
    /* Position cursor */
    write(1, buf, cursor);
}

static int readline_edit(const char *prompt, char *buf, int bufsize) {
    int len = 0, cursor = 0;
    g_hist_pos = g_hist_count;
    char saved_line[MAX_LINE] = {0};

    write_str(prompt);
    memset(buf, 0, bufsize);

    for (;;) {
        int c = read_char();
        if (c < 0) return -1;

        if (c == '\n' || c == '\r') {
            write_char('\n');
            buf[len] = '\0';
            return len;
        }

        if (c == 4 && len == 0) {  /* Ctrl+D on empty line */
            return -1;
        }

        if (c == 3) {  /* Ctrl+C */
            write_str("^C\n");
            buf[0] = '\0';
            return 0;
        }

        if (c == 127 || c == 8) {  /* Backspace */
            if (cursor > 0) {
                memmove(buf + cursor - 1, buf + cursor, len - cursor);
                cursor--;
                len--;
                buf[len] = '\0';
                redraw_line(prompt, buf, len, cursor);
            }
            continue;
        }

        if (c == 9) {  /* Tab - completion */
            buf[cursor] = '\0';
            /* Find word start */
            int wstart = cursor;
            while (wstart > 0 && buf[wstart - 1] != ' ') wstart--;
            char word[PATH_MAX_SH];
            int wlen = cursor - wstart;
            memcpy(word, buf + wstart, wlen);
            word[wlen] = '\0';

            static char results[MAX_COMPLETE][PATH_MAX_SH];
            int nresults;

            if (wstart == 0) {
                nresults = complete_commands(word, results, MAX_COMPLETE);
            } else {
                nresults = complete_files(word, results, MAX_COMPLETE);
            }

            if (nresults == 1) {
                int rlen = (int)strlen(results[0]);
                int insert = rlen - wlen;
                if (len + insert < bufsize - 1) {
                    memmove(buf + cursor + insert, buf + cursor, len - cursor);
                    memcpy(buf + wstart, results[0], rlen);
                    cursor += insert;
                    len += insert;
                    buf[len] = '\0';
                    redraw_line(prompt, buf, len, cursor);
                }
            } else if (nresults > 1) {
                write_char('\n');
                for (int i = 0; i < nresults && i < 20; i++) {
                    write_str(results[i]);
                    write_str("  ");
                }
                write_char('\n');
                buf[len] = '\0';
                write_str(prompt);
                write(1, buf, len);
            }
            continue;
        }

        if (c == 27) {  /* Escape sequence */
            int c2 = read_char();
            if (c2 == '[') {
                int c3 = read_char();
                switch (c3) {
                case 'A':  /* Up arrow - history */
                    if (g_hist_pos > 0 &&
                        g_hist_pos > g_hist_count - MAX_HISTORY) {
                        if (g_hist_pos == g_hist_count) {
                            memcpy(saved_line, buf, len + 1);
                        }
                        g_hist_pos--;
                        const char *h = hist_get(g_hist_pos);
                        if (h) {
                            strncpy(buf, h, bufsize - 1);
                            len = (int)strlen(buf);
                            cursor = len;
                            redraw_line(prompt, buf, len, cursor);
                        }
                    }
                    break;
                case 'B':  /* Down arrow */
                    if (g_hist_pos < g_hist_count) {
                        g_hist_pos++;
                        if (g_hist_pos == g_hist_count) {
                            memcpy(buf, saved_line, MAX_LINE);
                            len = (int)strlen(buf);
                        } else {
                            const char *h = hist_get(g_hist_pos);
                            if (h) {
                                strncpy(buf, h, bufsize - 1);
                                len = (int)strlen(buf);
                            }
                        }
                        cursor = len;
                        redraw_line(prompt, buf, len, cursor);
                    }
                    break;
                case 'C':  /* Right */
                    if (cursor < len) {
                        cursor++;
                        write_str("\033[C");
                    }
                    break;
                case 'D':  /* Left */
                    if (cursor > 0) {
                        cursor--;
                        write_str("\033[D");
                    }
                    break;
                case 'H':  /* Home */
                    cursor = 0;
                    redraw_line(prompt, buf, len, cursor);
                    break;
                case 'F':  /* End */
                    cursor = len;
                    redraw_line(prompt, buf, len, cursor);
                    break;
                case '3':  /* Delete key (ESC[3~) */
                    read_char();  /* consume '~' */
                    if (cursor < len) {
                        memmove(buf + cursor, buf + cursor + 1, len - cursor - 1);
                        len--;
                        buf[len] = '\0';
                        redraw_line(prompt, buf, len, cursor);
                    }
                    break;
                }
            }
            continue;
        }

        if (c == 1) {  /* Ctrl+A - home */
            cursor = 0;
            redraw_line(prompt, buf, len, cursor);
            continue;
        }
        if (c == 5) {  /* Ctrl+E - end */
            cursor = len;
            redraw_line(prompt, buf, len, cursor);
            continue;
        }
        if (c == 11) {  /* Ctrl+K - kill to end */
            len = cursor;
            buf[len] = '\0';
            redraw_line(prompt, buf, len, cursor);
            continue;
        }
        if (c == 21) {  /* Ctrl+U - kill to start */
            memmove(buf, buf + cursor, len - cursor);
            len -= cursor;
            cursor = 0;
            buf[len] = '\0';
            redraw_line(prompt, buf, len, cursor);
            continue;
        }
        if (c == 12) {  /* Ctrl+L - clear screen */
            write_str("\033[2J\033[H");
            write_str(prompt);
            write(1, buf, len);
            continue;
        }

        /* Printable character */
        if (c >= 32 && c < 127 && len < bufsize - 1) {
            memmove(buf + cursor + 1, buf + cursor, len - cursor);
            buf[cursor] = (char)c;
            cursor++;
            len++;
            buf[len] = '\0';
            if (cursor == len) {
                write_char((char)c);
            } else {
                redraw_line(prompt, buf, len, cursor);
            }
        }
    }
}

/* ---- Prompt generation ---- */

static void build_prompt(char *out, int outsize) {
    char cwd[PATH_MAX_SH];
    if (getcwd(cwd, sizeof(cwd)) == NULL) strcpy(cwd, "/");

    const char *user = getenv("USER");
    if (!user) user = "user";

    const char *home = getenv("HOME");
    char display_cwd[PATH_MAX_SH];

    if (home && strncmp(cwd, home, strlen(home)) == 0) {
        display_cwd[0] = '~';
        strncpy(display_cwd + 1, cwd + strlen(home), PATH_MAX_SH - 2);
    } else {
        strncpy(display_cwd, cwd, PATH_MAX_SH - 1);
    }
    display_cwd[PATH_MAX_SH - 1] = '\0';

    int uid = 0;  /* tobyOS: getuid not available in libtoby */
    char suffix = (uid == 0) ? '#' : '$';

    snprintf(out, outsize, "%s@tobyos:%s%c ", user, display_cwd, suffix);
}

/* ---- Main ---- */

int main(int argc, char **argv) {
    shell_set_pid(getpid());
    parser_init();

    /* Handle -c "command" */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        shell_set_interactive(0);
        return parser_execute_line(argv[2]);
    }

    /* Handle script file argument */
    if (argc >= 2 && argv[1][0] != '-') {
        shell_set_interactive(0);
        return parser_execute_file(argv[1]);
    }

    /* Interactive mode */
    shell_set_interactive(1);
    term_raw_enable();

    /* Source ~/.shrc if present */
    parser_execute_file("/etc/shrc");

    char line[MAX_LINE];
    char prompt[512];

    while (shell_is_running()) {
        build_prompt(prompt, sizeof(prompt));

        int n = readline_edit(prompt, line, MAX_LINE);
        if (n < 0) {
            write_str("exit\n");
            break;
        }
        if (line[0] == '\0') continue;

        hist_add(line);
        parser_execute_line(line);
    }

    term_raw_disable();
    return shell_get_last_status();
}
