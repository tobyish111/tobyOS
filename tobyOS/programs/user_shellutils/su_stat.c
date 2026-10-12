/* su_stat.c -- stat; included by main.c. */

/* GNU stat's %F names. libtoby's <sys/stat.h> used to define only S_ISDIR
 * and S_ISREG, so this said "unknown" for every /dev node -- which the
 * kernel reports as S_IFCHR -- while GNU says "character special file". */
static const char *type_name(mode_t m) {
    if (S_ISDIR(m))  return "directory";
    if (S_ISREG(m))  return "regular file";
    if (S_ISCHR(m))  return "character special file";
    if (S_ISBLK(m))  return "block special file";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISLNK(m))  return "symbolic link";
    if (S_ISSOCK(m)) return "socket";
    return "unknown";
}

static int do_stat(int argc, char **argv) {
    int rc = 0, terse = 0;
    const char *fmt = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--terse")) { terse = 1; continue; }
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { fmt = argv[++i]; continue; }
        if (!strncmp(argv[i], "--format=", 9)) { fmt = argv[i] + 9; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            fprintf(stderr, "stat: cannot stat '%s': %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (fmt) {
            for (const char *p = fmt; *p; p++) {
                if (*p != '%') { putchar(*p); continue; }
                p++;
                switch (*p) {
                case 'n': fputs(argv[i], stdout); break;
                case 's': printf("%ld", (long)st.st_size); break;
                case 'f': printf("%lx", (unsigned long)st.st_mode); break;
                case 'a': printf("%lo", (unsigned long)(st.st_mode & 07777)); break;
                case 'u': printf("%u", (unsigned)st.st_uid); break;
                case 'g': printf("%u", (unsigned)st.st_gid); break;
                case 'i': fputs("0", stdout); break;   /* no st_ino here */
                case 'F': fputs(type_name(st.st_mode), stdout); break;
                case '%': putchar('%'); break;
                default: putchar('%'); if (*p) putchar(*p); break;
                }
                if (!*p) break;
            }
            putchar('\n');
            continue;
        }
        if (terse) {
            printf("%s %ld %ld %lx %u %u\n", argv[i], (long)st.st_size,
                   (long)st.st_size, (unsigned long)st.st_mode,
                   (unsigned)st.st_uid, (unsigned)st.st_gid);
            continue;
        }
        printf("  File: %s\n", argv[i]);
        printf("  Size: %-14ld Blocks: %-10ld %s\n",
               (long)st.st_size, (long)((st.st_size + 511) / 512),
               type_name(st.st_mode));
        printf("Access: (%04lo)  Uid: (%5u)   Gid: (%5u)\n",
               (unsigned long)(st.st_mode & 07777),
               (unsigned)st.st_uid, (unsigned)st.st_gid);
    }
    return rc;
}
