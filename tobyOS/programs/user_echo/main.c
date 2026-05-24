/* user_echo/main.c -- print arguments to stdout.
 *
 * Usage: echo [-n] [-e] [string ...]
 *
 *   -n   do not output trailing newline
 *   -e   interpret backslash escape sequences:
 *          \\  backslash
 *          \n  newline
 *          \t  tab
 *          \r  carriage return
 *          \0  NUL byte
 *          \xHH  hex byte
 *
 * Bytes are emitted via SYS_WRITE on fd 1 (stdout). When run under a
 * shell pipeline ("echo hello | cat"), the shell rewires fd 1 to the
 * write end of a pipe before spawning us.
 */

typedef unsigned long size_t;
typedef long          ssize_t;

#define SYS_WRITE  1

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static int hexdig(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static void write_escaped(const char *s) {
    char outbuf[256];
    int oi = 0;

    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            char c;
            switch (*s) {
            case '\\': c = '\\'; s++; break;
            case 'n':  c = '\n'; s++; break;
            case 't':  c = '\t'; s++; break;
            case 'r':  c = '\r'; s++; break;
            case '0':  c = '\0'; s++; break;
            case 'a':  c = '\a'; s++; break;
            case 'b':  c = '\b'; s++; break;
            case 'x': {
                s++;
                int h1 = hexdig(*s);
                if (h1 < 0) { outbuf[oi++] = '\\'; c = 'x'; break; }
                s++;
                int h2 = hexdig(*s);
                if (h2 < 0) { c = (char)h1; break; }
                s++;
                c = (char)((h1 << 4) | h2);
                break;
            }
            default: outbuf[oi++] = '\\'; c = *s; s++; break;
            }
            outbuf[oi++] = c;
        } else {
            outbuf[oi++] = *s++;
        }
        if (oi >= 240) {
            sys_write(1, outbuf, (size_t)oi);
            oi = 0;
        }
    }
    if (oi > 0) sys_write(1, outbuf, (size_t)oi);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    int no_newline = 0;
    int interpret_escapes = 0;
    int first_arg = 1;

    while (first_arg < argc && argv[first_arg][0] == '-' &&
           argv[first_arg][1] != '\0') {
        const char *opt = argv[first_arg];
        int valid = 1;
        for (int j = 1; opt[j]; j++) {
            if (opt[j] == 'n') no_newline = 1;
            else if (opt[j] == 'e') interpret_escapes = 1;
            else if (opt[j] == 'E') interpret_escapes = 0;
            else { valid = 0; break; }
        }
        if (!valid) break;
        first_arg++;
    }

    for (int i = first_arg; i < argc; i++) {
        if (i > first_arg) sys_write(1, " ", 1);
        if (interpret_escapes) {
            write_escaped(argv[i]);
        } else {
            sys_write(1, argv[i], my_strlen(argv[i]));
        }
    }
    if (!no_newline) sys_write(1, "\n", 1);
    return 0;
}
