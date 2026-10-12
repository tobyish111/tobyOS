/* su_uname.c -- uname; included by main.c. */

static int do_uname(int argc, char **argv) {
    char sys[128] = "tobyOS", rel[128] = "", ver[512] = "", mach[64] = "x86_64", node[128] = "";
    char buf[1024];
    if (slurp("/proc/sys/kernel/ostype", buf, sizeof buf) > 0)
        snprintf(sys, sizeof sys, "%s", first_line(buf));
    if (slurp("/proc/sys/kernel/osrelease", buf, sizeof buf) > 0)
        snprintf(rel, sizeof rel, "%s", first_line(buf));
    if (slurp("/proc/sys/kernel/hostname", buf, sizeof buf) > 0)
        snprintf(node, sizeof node, "%s", first_line(buf));
    if (slurp("/proc/version", buf, sizeof buf) > 0)
        snprintf(ver, sizeof ver, "%s", first_line(buf));

    int s = 0, n = 0, r = 0, v = 0, m = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--all")) { s = n = r = v = m = 1; any = 1; continue; }
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'a': s = n = r = v = m = 1; break;
                case 's': s = 1; break;
                case 'n': n = 1; break;
                case 'r': r = 1; break;
                case 'v': v = 1; break;
                case 'm': case 'p': case 'i': m = 1; break;
                default: break;
                }
                any = 1;
            }
        }
    }
    if (!any) s = 1;
    int first = 1;
    #define OUT(x) do { if (*(x)) { if (!first) putchar(' '); fputs((x), stdout); first = 0; } } while (0)
    if (s) OUT(sys);
    if (n) OUT(node);
    if (r) OUT(rel);
    if (v) OUT(ver);
    if (m) OUT(mach);
    #undef OUT
    putchar('\n');
    return 0;
}
