/* su_free.c -- free; included by main.c. */

static long meminfo_kb(const char *buf, const char *key) {
    size_t klen = strlen(key);
    for (const char *p = buf; *p; ) {
        const char *eol = strchr(p, '\n');
        if (!strncmp(p, key, klen) && p[klen] == ':') {
            const char *q = p + klen + 1;
            while (*q == ' ' || *q == '\t') q++;
            long v = 0; int got = 0;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q++ - '0'); got = 1; }
            return got ? v : -1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return -1;
}

static int do_free(int argc, char **argv) {
    int human = 0, div = 1;
    const char *unit = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h")) { human = 1; continue; }
        if (!strcmp(argv[i], "-m")) { div = 1024; unit = "Mi"; continue; }
        if (!strcmp(argv[i], "-g")) { div = 1024 * 1024; unit = "Gi"; continue; }
        if (!strcmp(argv[i], "-k")) { div = 1; continue; }
    }
    char buf[8192];
    if (slurp("/proc/meminfo", buf, sizeof buf) < 0) {
        fprintf(stderr, "free: cannot read /proc/meminfo\n");
        return 1;
    }
    long total = meminfo_kb(buf, "MemTotal");
    long freek = meminfo_kb(buf, "MemFree");
    long avail = meminfo_kb(buf, "MemAvailable");
    long buff  = meminfo_kb(buf, "Buffers");
    long cache = meminfo_kb(buf, "Cached");
    long swtot = meminfo_kb(buf, "SwapTotal");
    long swfre = meminfo_kb(buf, "SwapFree");
    if (total < 0) total = 0;
    if (freek < 0) freek = 0;
    if (buff  < 0) buff  = 0;
    if (cache < 0) cache = 0;
    if (avail < 0) avail = freek;
    long used = total - freek - buff - cache;
    if (used < 0) used = 0;

    if (human) {
        /* Integer mebibytes: this program is built without SSE, so no
         * floating point. One decimal is not worth a soft-float call. */
        printf("%-14s%9s%9s%9s%9s%9s%9s\n",
               "", "total", "used", "free", "shared", "buff/cache", "available");
        printf("%-14s%8ldM%8ldM%8ldM%9s%9ldM%8ldM\n", "Mem:",
               total/1024, used/1024, freek/1024, "0", (buff+cache)/1024, avail/1024);
        printf("%-14s%8ldM%8ldM%8ldM\n", "Swap:",
               (swtot < 0 ? 0 : swtot)/1024,
               (swtot < 0 || swfre < 0 ? 0 : swtot - swfre)/1024,
               (swfre < 0 ? 0 : swfre)/1024);
        return 0;
    }
    printf("%-14s%12s%12s%12s%12s%12s%12s\n",
           "", "total", "used", "free", "shared", "buff/cache", "available");
    printf("%-14s%12ld%12ld%12ld%12d%12ld%12ld\n", "Mem:",
           total/div, used/div, freek/div, 0, (buff+cache)/div, avail/div);
    printf("%-14s%12ld%12ld%12ld\n", "Swap:",
           (swtot < 0 ? 0 : swtot)/div,
           (swtot < 0 || swfre < 0 ? 0 : (swtot - swfre))/div,
           (swfre < 0 ? 0 : swfre)/div);
    (void)unit;
    return 0;
}
